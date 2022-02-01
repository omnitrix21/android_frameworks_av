/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0

#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <libxml/parser.h>
#include <libxml/xinclude.h>
#include <string.h>
#include <system/audio_config.h>

#include "audio_test_utils.h"

using namespace android;

template <class T>
constexpr void (*xmlDeleter)(T* t);
template <>
constexpr auto xmlDeleter<xmlDoc> = xmlFreeDoc;
template <>
constexpr auto xmlDeleter<xmlChar> = [](xmlChar* s) { xmlFree(s); };

/** @return a unique_ptr with the correct deleter for the libxml2 object. */
template <class T>
constexpr auto make_xmlUnique(T* t) {
    // Wrap deleter in lambda to enable empty base optimization
    auto deleter = [](T* t) { xmlDeleter<T>(t); };
    return std::unique_ptr<T, decltype(deleter)>{t, deleter};
}

std::string getXmlAttribute(const xmlNode* cur, const char* attribute) {
    auto charPtr = make_xmlUnique(xmlGetProp(cur, reinterpret_cast<const xmlChar*>(attribute)));
    if (charPtr == NULL) {
        return "";
    }
    std::string value(reinterpret_cast<const char*>(charPtr.get()));
    return value;
}

struct MixPort {
    std::string name;
    std::string role;
    std::string flags;
};

struct Route {
    std::string name;
    std::string sources;
    std::string sink;
};

status_t parse_audio_policy_configuration_xml(std::vector<std::string>& attachedDevices,
                                              std::vector<MixPort>& mixPorts,
                                              std::vector<Route>& routes) {
    std::string path = audio_find_readable_configuration_file("audio_policy_configuration.xml");
    if (path.length() == 0) return UNKNOWN_ERROR;
    auto doc = make_xmlUnique(xmlParseFile(path.c_str()));
    if (doc == nullptr) return UNKNOWN_ERROR;
    xmlNode* root = xmlDocGetRootElement(doc.get());
    if (root == nullptr) return UNKNOWN_ERROR;
    if (xmlXIncludeProcess(doc.get()) < 0) return UNKNOWN_ERROR;
    mixPorts.clear();
    if (!xmlStrcmp(root->name, reinterpret_cast<const xmlChar*>("audioPolicyConfiguration"))) {
        std::string raw{getXmlAttribute(root, "version")};
        for (auto* child = root->xmlChildrenNode; child != nullptr; child = child->next) {
            if (!xmlStrcmp(child->name, reinterpret_cast<const xmlChar*>("modules"))) {
                xmlNode* root = child;
                for (auto* child = root->xmlChildrenNode; child != nullptr; child = child->next) {
                    if (!xmlStrcmp(child->name, reinterpret_cast<const xmlChar*>("module"))) {
                        xmlNode* root = child;
                        for (auto* child = root->xmlChildrenNode; child != nullptr;
                             child = child->next) {
                            if (!xmlStrcmp(child->name,
                                           reinterpret_cast<const xmlChar*>("mixPorts"))) {
                                xmlNode* root = child;
                                for (auto* child = root->xmlChildrenNode; child != nullptr;
                                     child = child->next) {
                                    if (!xmlStrcmp(child->name,
                                                   reinterpret_cast<const xmlChar*>("mixPort"))) {
                                        MixPort mixPort;
                                        xmlNode* root = child;
                                        mixPort.name = getXmlAttribute(root, "name");
                                        mixPort.role = getXmlAttribute(root, "role");
                                        mixPort.flags = getXmlAttribute(root, "flags");
                                        if (mixPort.role == "source") mixPorts.push_back(mixPort);
                                    }
                                }
                            } else if (!xmlStrcmp(child->name, reinterpret_cast<const xmlChar*>(
                                                                       "attachedDevices"))) {
                                xmlNode* root = child;
                                for (auto* child = root->xmlChildrenNode; child != nullptr;
                                     child = child->next) {
                                    if (!xmlStrcmp(child->name,
                                                   reinterpret_cast<const xmlChar*>("item"))) {
                                        auto xmlValue = make_xmlUnique(xmlNodeListGetString(
                                                child->doc, child->xmlChildrenNode, 1));
                                        if (xmlValue == nullptr) {
                                            raw = "";
                                        } else {
                                            raw = reinterpret_cast<const char*>(xmlValue.get());
                                        }
                                        std::string& value = raw;
                                        attachedDevices.push_back(std::move(value));
                                    }
                                }
                            } else if (!xmlStrcmp(child->name,
                                                  reinterpret_cast<const xmlChar*>("routes"))) {
                                xmlNode* root = child;
                                for (auto* child = root->xmlChildrenNode; child != nullptr;
                                     child = child->next) {
                                    if (!xmlStrcmp(child->name,
                                                   reinterpret_cast<const xmlChar*>("route"))) {
                                        Route route;
                                        xmlNode* root = child;
                                        route.name = getXmlAttribute(root, "name");
                                        route.sources = getXmlAttribute(root, "sources");
                                        route.sink = getXmlAttribute(root, "sink");
                                        routes.push_back(route);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return OK;
}

// UNIT TEST
TEST(AudioTrackTest, TestPerformanceMode) {
    std::vector<std::string> attachedDevices;
    std::vector<MixPort> mixPorts;
    std::vector<Route> routes;
    EXPECT_EQ(OK, parse_audio_policy_configuration_xml(attachedDevices, mixPorts, routes));
    std::string output_flags_string[] = {"AUDIO_OUTPUT_FLAG_FAST", "AUDIO_OUTPUT_FLAG_DEEP_BUFFER"};
    audio_output_flags_t output_flags[] = {AUDIO_OUTPUT_FLAG_FAST, AUDIO_OUTPUT_FLAG_DEEP_BUFFER};
    audio_flags_mask_t flags[] = {AUDIO_FLAG_LOW_LATENCY, AUDIO_FLAG_DEEP_BUFFER};
    bool hasFlag = false;
    for (int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        hasFlag = false;
        for (int j = 0; j < mixPorts.size() && !hasFlag; j++) {
            MixPort port = mixPorts[j];
            if (port.role == "source" && port.flags.find(output_flags_string[i]) != -1) {
                for (int k = 0; k < routes.size() && !hasFlag; k++) {
                    if (routes[k].sources.find(port.name) != -1 &&
                        std::find(attachedDevices.begin(), attachedDevices.end(), routes[k].sink) !=
                                attachedDevices.end()) {
                        hasFlag = true;
                        std::cerr << "found port with flag " << output_flags_string[i] << "@ "
                                  << " port :: name : " << port.name << " role : " << port.role
                                  << " port :: flags : " << port.flags
                                  << " connected via route name : " << routes[k].name
                                  << " route sources : " << routes[k].sources
                                  << " route sink : " << routes[k].sink << std::endl;
                    }
                }
            }
        }
        if (!hasFlag) continue;
        audio_attributes_t attributes = AUDIO_ATTRIBUTES_INITIALIZER;
        attributes.usage = AUDIO_USAGE_MEDIA;
        attributes.content_type = AUDIO_CONTENT_TYPE_MUSIC;
        attributes.flags = flags[i];
        std::unique_ptr<AudioPlayback> ap(new AudioPlayback(
                0, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO, AUDIO_OUTPUT_FLAG_NONE,
                AUDIO_SESSION_NONE, AudioTrack::TRANSFER_OBTAIN, &attributes));
        ASSERT_NE(nullptr, ap);
        ASSERT_EQ(OK, ap->loadResource("/data/local/tmp/bbb_2ch_24kHz_s16le.raw"))
                << "Unable to open Resource";
        EXPECT_EQ(OK, ap->create()) << "track creation failed";
        sp<OnAudioDeviceUpdateNotifier> cb = new OnAudioDeviceUpdateNotifier();
        EXPECT_EQ(OK, ap->getAudioTrackHandle()->addAudioDeviceCallback(cb));
        EXPECT_EQ(OK, ap->start()) << "audio track start failed";
        EXPECT_EQ(OK, ap->onProcess());
        EXPECT_EQ(OK, cb->waitForAudioDeviceCb());
        EXPECT_TRUE(checkPatchPlayback(cb->mAudioIo, cb->mDeviceId));
        EXPECT_NE(0, ap->getAudioTrackHandle()->getFlags() & output_flags[i]);
        audio_patch patch;
        EXPECT_EQ(OK, getPatchForOutputMix(cb->mAudioIo, patch));
        for (auto j = 0; j < patch.num_sources; j++) {
            if (patch.sources[j].type == AUDIO_PORT_TYPE_MIX &&
                patch.sources[j].ext.mix.handle == cb->mAudioIo) {
                if ((patch.sources[j].flags.output & output_flags[i]) == 0) {
                    ADD_FAILURE() << "expected output flag " << output_flags[i] << " is absent";
                    std::cerr << dumpPortConfig(patch.sources[j]);
                }
            }
        }
        ap->stop();
        ap->getAudioTrackHandle()->removeAudioDeviceCallback(cb);
    }
}

TEST(AudioTrackTest, TestRemoteSubmix) {
    std::vector<std::string> attachedDevices;
    std::vector<MixPort> mixPorts;
    std::vector<Route> routes;
    EXPECT_EQ(OK, parse_audio_policy_configuration_xml(attachedDevices, mixPorts, routes));
    bool hasFlag = false;
    for (int j = 0; j < attachedDevices.size() && !hasFlag; j++) {
        if (attachedDevices[j].find("Remote Submix") != -1) hasFlag = true;
    }
    if (!hasFlag) GTEST_SKIP() << " Device does not have Remote Submix port.";
    sp<AudioCapture> capture = new AudioCapture(AUDIO_SOURCE_REMOTE_SUBMIX, 48000,
                                                AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_IN_STEREO);
    ASSERT_NE(nullptr, capture);
    ASSERT_EQ(OK, capture->create()) << "record creation failed";

    std::unique_ptr<AudioPlayback> playback = std::make_unique<AudioPlayback>(
            48000, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO, AUDIO_OUTPUT_FLAG_NONE,
            AUDIO_SESSION_NONE);
    ASSERT_NE(nullptr, playback);
    ASSERT_EQ(OK, playback->loadResource("/data/local/tmp/bbb_2ch_24kHz_s16le.raw"))
            << "Unable to open Resource";
    ASSERT_EQ(OK, playback->create()) << "track creation failed";

    audio_port_v7 port;
    status_t status = getPortByAttributes(AUDIO_PORT_ROLE_SOURCE, AUDIO_PORT_TYPE_DEVICE,
                                          AUDIO_DEVICE_IN_REMOTE_SUBMIX, port);
    EXPECT_EQ(OK, status) << "Could not find port";

    EXPECT_EQ(OK, capture->start()) << "start recording failed";
    EXPECT_EQ(port.id, capture->getAudioRecordHandle()->getRoutedDeviceId())
            << "Capture NOT routed on expected port";

    status = getPortByAttributes(AUDIO_PORT_ROLE_SINK, AUDIO_PORT_TYPE_DEVICE,
                                 AUDIO_DEVICE_OUT_REMOTE_SUBMIX, port);
    EXPECT_EQ(OK, status) << "Could not find port";

    EXPECT_EQ(OK, playback->start()) << "audio track start failed";
    EXPECT_EQ(OK, playback->onProcess());
    ASSERT_EQ(port.id, playback->getAudioTrackHandle()->getRoutedDeviceId())
            << "Playback NOT routed on expected port";
    capture->stop();
    playback->stop();
}
