// SONOS::Squeezebox -- deploy Sonos in a Logitech Media Server (LMS) streaming environment
//
// Copyright (c) 2023 Martin van der Werff <github (at) newinnovations.nl>
//
// This file is part of SONOS::Squeezebox.
//
// SONOS::Squeezebox is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
// BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <contentdirectory.h>
#include <didlparser.h>
#include <filestreamer.h>
#include <imageservice.h>
#include <sonosplayer.h>
#include <sonossystem.h>

#include "sbstreamer.h"
#include "sonos-status.h"

extern "C" {
unsigned get_squeezebox_stream_id(void);
}

#include <algorithm>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

void squeezelite(const char* server, uint8_t * mac, const char* name);
static void handleEvent(void* handle);
static const char* getCmd(int argc, char** argv, const std::string& option);
static const char* getCmdOption(int argc, char** argv, const std::string& option);

SONOS::System* gSonos = 0;
SONOS::PlayerPtr gPlayer;
uint8_t gMac[6];
volatile bool gEvent = true;
static std::string gServer;
static std::string gLastTrackId;

static std::string urlEncode(std::string str)
{
    std::string new_str = "";
    for (auto& c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            new_str += c;
        } else {
            char bufHex[10];
            snprintf(bufHex, sizeof(bufHex) - 1, "%%%02x", c);
            new_str += bufHex;
        }
    }
    return new_str;
}

struct TrackInfo {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string artworkUrl;
};

static std::string lmsUrldecode(const std::string& s)
{
    std::string result;
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i + 1], s[i + 2], '\0' };
            result += (char)strtol(hex, nullptr, 16);
            i += 3;
        } else if (s[i] == '+') {
            result += ' ';
            ++i;
        } else {
            result += s[i++];
        }
    }
    return result;
}

// Query LMS CLI (port 9090) for current track metadata of player identified by mac.
// Returns empty fields on any error so the caller can gracefully fall back.
static TrackInfo fetchLmsTrackInfo(const std::string& server, const uint8_t* mac)
{
    TrackInfo info;
    if (server.empty()) return info;

    // Strip optional :port suffix to get the bare hostname / IP
    std::string host = server;
    size_t col = host.rfind(':');
    if (col != std::string::npos) {
        std::string tail = host.substr(col + 1);
        bool allDigits = !tail.empty();
        for (char c : tail) allDigits = allDigits && isdigit((unsigned char)c);
        if (allDigits) host = host.substr(0, col);
    }

    struct addrinfo hints = {}, *ai = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), "9090", &hints, &ai) != 0) return info;

    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { freeaddrinfo(ai); return info; }

    struct timeval tv { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    bool ok = (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0);
    freeaddrinfo(ai);
    if (!ok) { close(fd); return info; }

    // LMS CLI expects the raw MAC with literal colons (xx:xx:xx:xx:xx:xx)
    char macFmt[18];
    snprintf(macFmt, sizeof(macFmt), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // tags: a=artist, l=album; title and id are always returned
    std::string query = std::string(macFmt) + " status - 1 tags:aAl\n";
    send(fd, query.c_str(), query.size(), 0);

    std::string response;
    char ch;
    while (response.size() < 8192 && recv(fd, &ch, 1, 0) == 1 && ch != '\n')
        response += ch;
    close(fd);

    printf("LMS response: %s\n", response.c_str());

    // Each space-separated token is URL-encoded "key:value"
    std::istringstream ss(response);
    std::string token;
    while (ss >> token) {
        std::string decoded = lmsUrldecode(token);
        size_t sep = decoded.find(':');
        if (sep == std::string::npos) continue;
        std::string key = decoded.substr(0, sep);
        std::string val = decoded.substr(sep + 1);
        if      (key == "title")  info.title = val;
        else if (key == "artist") info.artist = val;
        else if (key == "album")  info.album = val;
        else if (key == "id")     info.id = val;
    }

    // Build cover art URL from track id (works for local library and most streams)
    if (!info.id.empty())
        info.artworkUrl = "http://" + host + ":9000/music/" + info.id + "/cover.jpg";

    return info;
}

void squeezelite_thread(const char* server, std::string name)
{
    squeezelite(server, gMac, name.c_str());
    printf("squeezelite_thread: stopped\n");
}

bool PlaySqueezeBox(unsigned stream_id)
{
    SONOS::RequestBroker::ResourcePtr res(nullptr);
    SONOS::RequestBrokerPtr rb = gSonos->GetRequestBroker(SBSTREAMER_CNAME);
    if (rb) {
        res = rb->GetResource(SBSTREAMER_CNAME);
    }
    if (res) {
        bool hasParam = (res->uri.find("?") != std::string::npos);
        std::string streamURL;
        streamURL.assign(gPlayer->GetControllerUri()).append(res->uri).append(hasParam ? "&" : "?").append("stream=" + std::to_string(stream_id));
        std::string iconURL;
        iconURL.assign(gPlayer->GetControllerUri()).append(res->iconUri);

        TrackInfo track = fetchLmsTrackInfo(gServer, gMac);
        std::string title  = track.title.empty()      ? "Squeezebox"  : track.title;
        std::string artUrl = track.artworkUrl.empty() ? iconURL       : track.artworkUrl;

        // Prime the ICY title and track-id baseline for the new stream
        if (!track.id.empty()) {
            gLastTrackId = track.id;
            std::string icy = track.artist.empty() ? track.title : track.artist + " - " + track.title;
            set_squeezebox_icy_title(icy);
        }

        printf("PlaySqueezeBox: title='%s' art='%s'\n", title.c_str(), artUrl.c_str());
        return gPlayer->PlayStream(streamURL, title, artUrl);
    }
    printf("%s: service unavailable\n", __FUNCTION__);
    return false;
}

void flac_test();

int main(int argc, char** argv)
{
    int ret = 0, debug_level = 0;

    if (getCmd(argc, argv, "--debug"))
        debug_level = 4;

    const char* ip = getCmdOption(argc, argv, "--ip");
    const char* room = getCmdOption(argc, argv, "--room");
    const char* filename = getCmdOption(argc, argv, "--file");
    const char* server = getCmdOption(argc, argv, "--server");

    printf("\n\n| SONOS::Squeezebox -- deploy Sonos in a Logitech Media Server (LMS) streaming environment\n|\n");
    printf("| Copyright (c) 2023 Martin van der Werff <github (at) newinnovations.nl>\n\n\n");

    SONOS::System::Debug(debug_level);

    gSonos = new SONOS::System(0, handleEvent);

    if (!ip) {
        printf("Connecting to Sonos ... ");
        fflush(stdout);
        if (!gSonos->Discover()) {
            printf("No devices found (try specifying known Sonos player ip-address).\n");
            return EXIT_FAILURE;
        } else {
            printf("SUCCESS\n\n");
        }
    } else {
        std::string deviceUrl = "http://" + std::string(ip) + ":1400";
        printf("Connecting to Sonos (through player %s) ... ", ip);
        fflush(stdout);
        if (!gSonos->Discover(deviceUrl)) {
            printf("Device is unreachable.\n");
            return EXIT_FAILURE;
        } else {
            printf("SUCCESS\n\n");
        }
    }

    {
        SONOS::RequestBrokerPtr imageService(new SONOS::ImageService());
        gSonos->RegisterRequestBroker(imageService);
        gSonos->RegisterRequestBroker(SONOS::RequestBrokerPtr(new SONOS::SBStreamer(imageService.get())));
        gSonos->RegisterRequestBroker(SONOS::RequestBrokerPtr(new SONOS::FileStreamer()));
    }

    printf("+----------------------------------------------------------------------- devices / players ---+\n");
    printf("| %-35s | %-24s | %-18s | %5s |\n", "player name", "uuid", "host", "port");
    printf("+---------------------------------------------------------------------------------------------+\n");
    SONOS::ZonePlayerList players = gSonos->GetZonePlayerList();
    for (SONOS::ZonePlayerList::const_iterator it = players.begin(); it != players.end(); ++it) {
        printf("| %-35s | %-24s | %-18s | %5d |\n",
            it->first.c_str(), it->second->GetUUID().c_str(), it->second->GetHost().c_str(), it->second->GetPort());
    }
    printf("+---------------------------------------------------------------------------------------------+\n\n");

    printf("+--------------------------------------------------------------------------- zones / rooms ---+\n");
    printf("| %-35s | %-53s |\n", "room name", "coordinating player");
    printf("+---------------------------------------------------------------------------------------------+\n");
    SONOS::ZoneList zones = gSonos->GetZoneList();
    for (SONOS::ZoneList::const_iterator it = zones.begin(); it != zones.end(); ++it) {
        printf("| %-35s | %-53s |\n",
            it->second->GetZoneName().c_str(), it->second->GetCoordinator()->c_str());
    }
    printf("+---------------------------------------------------------------------------------------------+\n\n");

    if (!room) {
        printf("Please specify a room to join with the --room option\n");
        return EXIT_FAILURE;
    }

    printf("Connecting to room %s ... ", room);

    std::string zone = room;
    bool found = false;
    for (SONOS::ZoneList::const_iterator iz = zones.begin(); iz != zones.end(); ++iz) {
        if (iz->second->GetZoneName() == zone) { // zone == room
            found = true;
            if ((gPlayer = gSonos->GetPlayer(iz->second, 0, handleEvent))) {
                printf("SUCCESS");
            } else {
                printf("FAILED to connect\n");
                return EXIT_FAILURE;
            }
            break;
        }
    }
    if (!found) {
        printf("FAILED to find room\n");
        return EXIT_FAILURE;
    }

    SONOS::Status status(gPlayer);
    status.get_mac(gMac);
    printf(" (MAC = %02X:%02X:%02X:%02X:%02X:%02X)\n\n", gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
    gServer = server ? server : "";

    std::thread* t = 0;
    if (filename) {
        std::string fn(filename);
        std::string extension("none");
        std::string::size_type idx = fn.rfind('.');

        if (idx != std::string::npos) {
            extension = fn.substr(idx + 1);
        }

        std::string url = gPlayer->GetControllerUri() + "/music/track." + extension + "?path=" + urlEncode(fn);

        if (gPlayer->PlayStream(url, "")) {
            printf("Started playing URL %s\n", url.c_str());
        } else {
            printf("Failed to start URL %s\n", url.c_str());
        }
    } else {
        std::string name = "SONOS::" + std::string(room);
        t = new std::thread(squeezelite_thread, server, name);
    }

    unsigned current_stream_id = 0;
    unsigned time_count = 0;
    unsigned meta_count = 0;

    status.update();

    for (;;) {
        unsigned stream_id = get_squeezebox_stream_id();
        if (stream_id != current_stream_id) {
            current_stream_id = stream_id;
            PlaySqueezeBox(stream_id);
            meta_count = 0; // reset poll timer after a stream restart
        }

        // Poll LMS every 5 s for track changes during gapless playback
        if (++meta_count >= 500 && !gServer.empty()) {
            meta_count = 0;
            TrackInfo t = fetchLmsTrackInfo(gServer, gMac);
            if (!t.id.empty() && t.id != gLastTrackId) {
                gLastTrackId = t.id;
                std::string icy = t.artist.empty() ? t.title : t.artist + " - " + t.title;
                set_squeezebox_icy_title(icy);
                printf("Track changed (gapless): id=%s icy='%s'\n", t.id.c_str(), icy.c_str());
            }
        }

        if ((time_count == 3000) || gEvent) {
            gEvent = false;
            status.update();
            if (status.changed()) {
                status.print();
            }
            time_count = 0;
        } else {
            ++time_count;
        }
        usleep(10000); // 10ms
    }

    if (t) {
        t->join();
    }

    return ret;
}

static void handleEvent(void* handle)
{
    gEvent = true;
}

static const char* getCmd(int argc, char** argv, const std::string& option)
{
    char** end = argv + argc;
    char** itr = std::find(argv, end, option);
    if (itr != end) {
        return *itr;
    }
    return NULL;
}

static const char* getCmdOption(int argc, char** argv, const std::string& option)
{
    char** end = argv + argc;
    for (char** it = argv; it != end; ++it) {
        if (strncmp(*it, option.c_str(), option.length()) == 0 && (*it)[option.length()] == '=')
            return &((*it)[option.length() + 1]);
    }
    return NULL;
}
