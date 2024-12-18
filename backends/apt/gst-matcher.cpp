/* gst-matcher.cpp - Match GStreamer packages
 *
 * Copyright (c) 2010-2016 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gst-matcher.h"
#include "apt-utils.h"

#include <regex.h>
#include <gst/gst.h>

static bool inited = false;

GstMatcher::GstMatcher(gchar **values)
{
    if (!inited) {
        gst_init(NULL, NULL);
        inited = true;
    }

    // The search term from PackageKit daemon:
    // gstreamer0.10(urisource-foobar)
    // gstreamer0.10(decoder-audio/x-wma)(wmaversion=3)
    const char *pkreg = "^gstreamer\\(0.10\\|1\\)\\(\\.0\\)\\?"
                        "(\\(encoder\\|decoder\\|urisource\\|urisink\\|element\\)-\\([^)]\\+\\))"
                        "\\((.*)\\)\\?";

    regex_t pkre;
    if (regcomp(&pkre, pkreg, 0) != 0) {
        g_debug("Regex compilation error: %s", pkreg);
        return;
    }

    gchar *value;
    for (uint i = 0; i < g_strv_length(values); ++i) {
        value = values[i];
        regmatch_t matches[6];
        if (regexec(&pkre, value, 6, matches, 0) != REG_NOMATCH) {
            Match values;
            string version, type, data, opt, arch;

            // Appends the version "0.10"
            version = "\nGstreamer-Version: ";
            version.append(string(value, matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so));

            // type (encode|decoder...)
            type = string(value, matches[3].rm_so, matches[3].rm_eo - matches[3].rm_so);

            // data "audio/x-wma"
            data = string(value, matches[4].rm_so, matches[4].rm_eo - matches[4].rm_so);

            // opt "wmaversion=3"
            if (matches[5].rm_so != -1) {
                // remove the '(' ')' that the regex matched
                opt = string(value, matches[5].rm_so + 1, matches[5].rm_eo - matches[5].rm_so - 2);
                if (!opt.empty()) {
                    size_t start_pos = 0;
                    // This is hardcoded in pk-gstreamer-install, so we also hardcode it here
                    const string x86_64 = ")(64bit";

                    if (ends_with(opt.c_str(), x86_64.c_str())) {
                            // We hardcode 64bit -> amd64 here
                            arch = "amd64";
                            opt.erase(opt.end() - x86_64.length(), opt.end());
                    }

                    // Replace all ")(" with "," - convert from input to serialized caps format
                    while ((start_pos = opt.find(")(", start_pos)) != string::npos) {
                        if (start_pos == opt.length()-2) {
                            // Avoid trailing comma
                            opt.erase(start_pos, 2);
                            break;
                        }
                        opt.replace(start_pos, 2, ",");
                        start_pos++;
                    }
                }
            }

            if (type.compare("encoder") == 0) {
                type = "Gstreamer-Encoders: ";
            } else if (type.compare("decoder") == 0) {
                type = "Gstreamer-Decoders: ";
            } else if (type.compare("urisource") == 0) {
                type = "Gstreamer-Uri-Sources: ";
            } else if (type.compare("urisink") == 0) {
                type = "Gstreamer-Uri-Sinks: ";
            } else if (type.compare("element") == 0) {
                type = "Gstreamer-Elements: ";
            }

            gchar *capsString;
            if (opt.empty()) {
                capsString = g_strdup_printf("%s", data.c_str());
            } else {
                capsString = g_strdup_printf("%s, %s", data.c_str(), opt.c_str());
            }
            GstCaps *caps = gst_caps_from_string(capsString);
            g_free(capsString);

            if (caps == NULL) {
                continue;
            }

            values.version = version;
            values.type    = type;
            values.data    = data;
            values.opt     = opt;
            values.caps    = caps;
            values.arch    = arch;

            m_matches.push_back(values);
        } else {
            g_debug("gstmatcher: Did not match: %s", value);
        }
    }
    regfree(&pkre);
}

GstMatcher::~GstMatcher()
{
    for (const Match &match : m_matches) {
        gst_caps_unref(static_cast<GstCaps*>(match.caps));
    }
}

bool GstMatcher::matches(string record, string arch)
{
    for (const Match &match : m_matches) {
        // Tries to find "Gstreamer-version: xxx"
        if (record.find(match.version) != string::npos) {
            size_t found;
            if (!match.arch.empty() && arch != match.arch)
                    continue;
            found = record.find(match.type);
            // Tries to find the type "Gstreamer-Uri-Sinks: "
            if (found != string::npos) {
                found += match.type.size(); // skips the "Gstreamer-Uri-Sinks: " string
                size_t endOfLine;
                endOfLine = record.find('\n', found);

                GstCaps *caps;
                caps = gst_caps_from_string(record.substr(found, endOfLine - found).c_str());
                if (caps == NULL) {
                    continue;
                }

                // if the record is capable of intersect them we found the package
                bool provides = gst_caps_can_intersect(static_cast<GstCaps*>(match.caps), caps);
                gst_caps_unref(caps);

                if (provides) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool GstMatcher::hasMatches() const
{
    return !m_matches.empty();
}
