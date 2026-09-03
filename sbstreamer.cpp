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

#include "sbstreamer.h"

#include "data/datareader.h"
#include "imageservice.h"
#include "private/tokenizer.h"
#include "private/urlencoder.h"
#include "requestbroker.h"
#include "sbencoder.h"

#include <cstring>
#include <mutex>
#include <unistd.h>

#define SBSTREAMER_ICON "/pulseaudio.png"
#define SBSTREAMER_CONTENT "audio/flac"
#define SBSTREAMER_DESC "Audio stream from %s"
#define SBSTREAMER_TIMEOUT 10000
#define SBSTREAMER_MAX_PLAYBACK 3
#define SBSTREAMER_CHUNK 16384
#define ICY_METAINT 8192

using namespace NSROOT;

volatile SBEncoder* g_enc = 0;
std::mutex g_enc_mutex;

static std::mutex g_icy_mutex;
static std::string g_icy_title;

void set_squeezebox_icy_title(const std::string& title)
{
    std::lock_guard<std::mutex> lock(g_icy_mutex);
    g_icy_title = title;
}

// Returns an ICY metadata block ready for in-stream injection.
// Empty title produces the single 0x00 "no update" byte.
static std::string makeIcyBlock(const std::string& title)
{
    if (title.empty())
        return std::string(1, '\0');

    // Escape single quotes so StreamTitle='...'; isn't broken by the content
    std::string escaped;
    escaped.reserve(title.size());
    for (char c : title)
        if (c == '\'') { escaped += '\\'; escaped += '\''; } else escaped += c;

    std::string meta = "StreamTitle='" + escaped + "';";
    size_t nblocks = (meta.size() + 15) / 16;
    std::string block(1, static_cast<char>(nblocks));
    block += meta;
    block.resize(1 + nblocks * 16, '\0');
    return block;
}

extern "C" {
void encode_squeezebox_audio(const char* data, int len)
{
    int count = 0;
    g_enc_mutex.lock();
    for (;;) {
        if (g_enc) {
            int written = ((SBEncoder*)g_enc)->write(data, len, SBSTREAMER_TIMEOUT);
            if (written != len) {
                printf("encode_squeezebox_audio: write() failed %d != %d\n", written, len);
            }
            g_enc_mutex.unlock();
            return;
        } else {
            g_enc_mutex.unlock();
            if (count++ > 100) { // 10s
                printf("encode_squeezebox_audio: timeout waiting for stream request\n");
                return;
            }
            usleep(100000); // 100 ms
            g_enc_mutex.lock();
        }
    }
}

void close_squeezebox_audio()
{
    g_enc_mutex.lock();
    if (g_enc) {
        ((SBEncoder*)g_enc)->write(0, 0, SBSTREAMER_TIMEOUT);
    }
    g_enc_mutex.unlock();
}
} // extern "C"

SBStreamer::SBStreamer(RequestBroker* imageService /*= nullptr*/)
    : RequestBroker()
    , m_resources()
    , m_playbackCount(0)
{
    ResourcePtr img(nullptr);
    if (imageService) {
        img = imageService->RegisterResource(SBSTREAMER_CNAME, "Icon for " SBSTREAMER_CNAME,
            SBSTREAMER_ICON, DataReader::Instance());
    }
    ResourcePtr ptr = ResourcePtr(new Resource());
    ptr->uri = SBSTREAMER_URI;
    ptr->title = SBSTREAMER_CNAME;
    ptr->description = SBSTREAMER_DESC;
    ptr->contentType = SBSTREAMER_CONTENT;
    if (img) {
        ptr->iconUri.assign(img->uri).append("?id=" LIBVERSION);
    }
    m_resources.push_back(ptr);
}

bool SBStreamer::HandleRequest(handle* handle)
{
    if (!IsAborted()) {
        const std::string& requrl = RequestBroker::GetRequestURI(handle);
        if (requrl.compare(0, strlen(SBSTREAMER_URI), SBSTREAMER_URI) == 0) {
            switch (RequestBroker::GetRequestMethod(handle)) {
            case RequestBroker::Method_GET: {
                std::vector<std::string> params;
                readParameters(requrl, params);
                int stream = atoi(getParamValue(params, "stream").c_str());
                streamSqueezeBox(handle, stream);
                return true;
            }
            case RequestBroker::Method_HEAD: {
                std::string resp;
                resp.assign(RequestBroker::MakeResponseHeader(RequestBroker::Status_OK))
                    .append("Content-Type: audio/flac\r\n")
                    .append("\r\n");
                RequestBroker::Reply(handle, resp.c_str(), resp.length());
                return true;
            }
            default:
                return false; // unhandled method
            }
        }
    }
    return false;
}

RequestBroker::ResourcePtr SBStreamer::GetResource(const std::string& title)
{
    (void)title;
    return m_resources.front();
}

RequestBroker::ResourceList SBStreamer::GetResourceList()
{
    ResourceList list;
    for (ResourceList::iterator it = m_resources.begin(); it != m_resources.end(); ++it)
        list.push_back((*it));
    return list;
}

RequestBroker::ResourcePtr SBStreamer::RegisterResource(const std::string& title,
    const std::string& description,
    const std::string& path,
    StreamReader* delegate)
{
    (void)title;
    (void)description;
    (void)path;
    (void)delegate;
    return ResourcePtr();
}

void SBStreamer::UnregisterResource(const std::string& uri)
{
    (void)uri;
}

// Send one HTTP chunk: hex-size CRLF data CRLF
bool SBStreamer::sendChunk(handle* h, const char* data, size_t size)
{
    char hdr[16];
    int hlen = snprintf(hdr, sizeof(hdr), "%x\r\n", (unsigned)size);
    return RequestBroker::Reply(h, hdr, hlen)
        && RequestBroker::Reply(h, data, size)
        && RequestBroker::Reply(h, "\r\n", 2);
}

void SBStreamer::streamSqueezeBox(handle* handle, int stream)
{
    printf("Sonos requested stream %d\n", stream);

    m_playbackCount.Add(1);

    if (m_playbackCount.Load() > SBSTREAMER_MAX_PLAYBACK) {
        printf("ERROR: overloaded http (load=%d)\n", m_playbackCount.Load());
        Reply429(handle);
    } else {
        // --- Diagnostic: log all relevant request headers ---
        std::string icyReq = GetRequestHeader(handle, "Icy-MetaData");
        printf("stream %d: Icy-MetaData header = '%s'\n", stream, icyReq.c_str());

        // ICY injection is ONLY enabled when the client explicitly asked for it.
        // Any other value (empty, "0", absent) leaves the FLAC stream untouched.
        bool sendIcy = (icyReq == "1");
        printf("stream %d: ICY injection %s\n", stream, sendIcy ? "ENABLED" : "DISABLED - stream will be plain FLAC");

        std::string resp;
        resp.assign(RequestBroker::MakeResponseHeader(RequestBroker::Status_OK))
            .append("Content-Type: audio/flac\r\n")
            .append("Transfer-Encoding: chunked\r\n");
        if (sendIcy)
            resp.append("icy-metaint: " + std::to_string(ICY_METAINT) + "\r\n");
        resp.append("\r\n");

        // --- Diagnostic: log the response headers (replace CRLF for readability) ---
        {
            std::string logResp = resp;
            for (size_t i = 0; i + 1 < logResp.size(); ++i)
                if (logResp[i] == '\r' && logResp[i+1] == '\n') { logResp[i] = '|'; logResp.erase(i+1, 1); }
            printf("stream %d: response headers: %s\n", stream, logResp.c_str());
        }

        if (RequestBroker::Reply(handle, resp.c_str(), resp.length())) {
            SBEncoder* enc = new SBEncoder(stream);
            enc->open();
            {
                g_enc_mutex.lock();
                if (g_enc) {
                    if (((SBEncoder*)g_enc)->streamId() == stream) {
                        g_enc_mutex.unlock();
                        printf("Sonos requested stream that is already playing -- rejecting this request\n");
                        Reply429(handle);
                        m_playbackCount.Sub(1);
                        return;
                    } else {
                        ((SBEncoder*)g_enc)->close();
                    }
                }
                g_enc = enc;
                g_enc_mutex.unlock();
            }

            char* buf = new char[SBSTREAMER_CHUNK];
            int r = 0;
            // icyOffset: audio bytes since the last ICY block (resets to 0 after each block).
            // totalAudio: monotonically increasing audio-byte counter for logging.
            size_t icyOffset  = 0;
            size_t totalAudio = 0;
            size_t icyBlocks  = 0;

            while (!IsAborted() && (r = enc->read(buf, SBSTREAMER_CHUNK, SBSTREAMER_TIMEOUT)) > 0) {
                if (!sendIcy) {
                    // Plain path: FLAC bytes are sent exactly as the encoder produced them.
                    if (!sendChunk(handle, buf, (size_t)r))
                        break;
                } else {
                    // ICY path: split at exact ICY_METAINT boundaries.
                    int pos = 0;
                    bool ok = true;
                    while (pos < r && ok) {
                        size_t toMeta = ICY_METAINT - icyOffset;
                        size_t avail  = (size_t)(r - pos);
                        size_t n      = (avail < toMeta) ? avail : toMeta;

                        ok = sendChunk(handle, buf + pos, n);
                        pos        += (int)n;
                        icyOffset  += n;
                        totalAudio += n;

                        if (icyOffset == ICY_METAINT) {
                            std::string title;
                            {
                                std::lock_guard<std::mutex> lock(g_icy_mutex);
                                title = g_icy_title;
                            }
                            std::string block = makeIcyBlock(title);

                            // Log the first 5 blocks and then every 500th (throttle after initial burst)
                            if (icyBlocks < 5 || icyBlocks % 500 == 0) {
                                printf("ICY block #%zu audio_offset=%zu block_size=%zu"
                                       " bytes[0..3]=%02x %02x %02x %02x title='%s'\n",
                                    icyBlocks, totalAudio, block.size(),
                                    (unsigned char)block[0],
                                    block.size() > 1 ? (unsigned char)block[1] : 0,
                                    block.size() > 2 ? (unsigned char)block[2] : 0,
                                    block.size() > 3 ? (unsigned char)block[3] : 0,
                                    title.c_str());
                            }

                            ok = ok && sendChunk(handle, block.data(), block.size());
                            icyOffset = 0;
                            ++icyBlocks;
                        }
                    }
                    if (!ok)
                        break;
                }
            }

            printf("stream %d: done. audio_bytes=%zu icy_blocks=%zu\n",
                stream, totalAudio, icyBlocks);

            RequestBroker::Reply(handle, "0\r\n\r\n", 5);
            {
                g_enc_mutex.lock();
                enc->close();
                if (g_enc == enc)
                    g_enc = 0;
                g_enc_mutex.unlock();
            }
            delete enc;
            delete[] buf;
        }
    }

    m_playbackCount.Sub(1);
    printf("Done serving stream %d to Sonos\n", stream);
}

void SBStreamer::Reply400(handle* handle)
{
    std::string resp;
    resp.append(RequestBroker::MakeResponseHeader(RequestBroker::Status_Bad_Request)).append("\r\n");
    RequestBroker::Reply(handle, resp.c_str(), resp.length());
}

void SBStreamer::Reply429(handle* handle)
{
    std::string resp;
    resp.append(RequestBroker::MakeResponseHeader(RequestBroker::Status_Too_Many_Requests)).append("\r\n");
    RequestBroker::Reply(handle, resp.c_str(), resp.length());
}

void SBStreamer::readParameters(const std::string& streamUrl, std::vector<std::string>& params)
{
    size_t s = streamUrl.find('?');
    if (s != std::string::npos) {
        tokenize(streamUrl.substr(s + 1), "&", params, true);
    }
}

std::string SBStreamer::getParamValue(const std::vector<std::string>& params, const std::string& name)
{
    size_t lval = name.length() + 1;
    for (const std::string& str : params) {
        if (str.length() > lval && str.at(name.length()) == '=' && str.compare(0, name.length(), name) == 0) {
            return urldecode(str.substr(lval));
        }
    }
    return std::string();
}
