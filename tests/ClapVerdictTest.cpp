// HOT-STEP LINK -- ClapVerdictTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Checks the CLAP state-load verdict (clapLastStateLoadAccepted): a valid state is accepted,
// empty, truncated, random and foreign blobs are rejected, and a reject leaves every
// parameter value exactly where it was. Same-tag XML with an unknown version attribute is
// accepted, because loadState only checks the root tag name.
#include "PluginProcessor.h"

#include <cstdio>
#include <vector>

namespace
{
int failures = 0, passes = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? passes : failures)++;
}

std::vector<float> snapshot (HotStepLinkProcessor& p)
{
    std::vector<float> v;
    for (auto* prm : p.getParameters())
        v.push_back (prm->getValue());
    return v;
}

void setNonDefault (HotStepLinkProcessor& p)
{
    int i = 0;
    for (auto* prm : p.getParameters())
    {
        const float d = prm->getDefaultValue();
        prm->setValueNotifyingHost (d < 0.5f ? 0.9f : 0.1f);
        if (++i >= 4) break;
    }
}

bool sameValues (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return false;
    return true;
}

void printFirst (const char* label, const std::vector<float>& v)
{
    std::printf ("  %s:", label);
    for (size_t i = 0; i < v.size() && i < 4; ++i)
        std::printf (" %.4f", v[i]);
    std::printf ("\n");
}

// Loads blob, reports verdict, checks the parameters did not move.
void rejectCase (HotStepLinkProcessor& p, const void* data, int size, const char* name)
{
    p.setStateInformation (nullptr, 0); // reset verdict path is exercised below anyway
    const juce::MemoryBlock good = [&] { juce::MemoryBlock m; p.getStateInformation (m); return m; }();
    p.setStateInformation (good.getData(), (int) good.getSize());
    const bool before = p.clapLastStateLoadAccepted();
    const auto pre = snapshot (p);
    p.setStateInformation (data, size);
    const bool verdict = p.clapLastStateLoadAccepted();
    const auto post = snapshot (p);
    std::printf ("  %s: verdict before=%d after=%d size=%d\n", name, (int) before, (int) verdict, size);
    printFirst ("pre ", pre);
    printFirst ("post", post);
    char msg[160];
    std::snprintf (msg, sizeof (msg), "%s rejected (verdict false)", name);
    check (before && ! verdict, msg);
    std::snprintf (msg, sizeof (msg), "%s leaves all %d parameters unchanged", name, (int) pre.size());
    check (pre.size() >= 1 && sameValues (pre, post), msg);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    HotStepLinkProcessor p;
    setNonDefault (p);
    const auto origin = snapshot (p);
    std::printf ("parameters: %d\n", (int) origin.size());
    printFirst ("origin", origin);

    juce::MemoryBlock valid;
    p.getStateInformation (valid);
    auto xml = juce::AudioProcessor::getXmlFromBinary (valid.getData(), (int) valid.getSize());
    const juce::String tag = xml != nullptr ? xml->getTagName() : juce::String();
    std::printf ("valid state: %d bytes, root tag '%s'\n", (int) valid.getSize(), tag.toRawUTF8());

    // 1. Valid state into a fresh processor: accepted, values restored.
    {
        HotStepLinkProcessor q;
        q.setStateInformation (nullptr, 0);
        const bool afterEmpty = q.clapLastStateLoadAccepted();
        q.setStateInformation (valid.getData(), (int) valid.getSize());
        const bool v = q.clapLastStateLoadAccepted();
        const auto got = snapshot (q);
        printFirst ("restored", got);
        std::printf ("  fresh: verdict after empty=%d after valid=%d\n", (int) afterEmpty, (int) v);
        check (! afterEmpty && v, "valid state accepted (verdict true after a prior reject)");
        check (sameValues (origin, got), "valid state restores the non-default parameter values");
    }

    // 2. Reject cases on p (which currently holds the non-default values).
    rejectCase (p, nullptr, 0, "empty blob");
    rejectCase (p, valid.getData(), (int) valid.getSize() / 2, "truncated blob (half)");
    {
        std::vector<unsigned char> junk (512);
        uint32_t s = 0x1234567u;
        for (auto& b : junk) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; b = (unsigned char) s; }
        rejectCase (p, junk.data(), (int) junk.size(), "random bytes");
    }
    {
        juce::XmlElement foreign ("SOME_OTHER_PLUGIN");
        foreign.setAttribute ("gain", 0.25);
        juce::MemoryBlock fb;
        juce::AudioProcessor::copyXmlToBinary (foreign, fb);
        rejectCase (p, fb.getData(), (int) fb.getSize(), "foreign plugin XML");
    }
    {
        // Our raw XML text without the binary header: not a JUCE state chunk.
        const auto text = xml != nullptr ? xml->toString() : juce::String ("x");
        rejectCase (p, text.toRawUTF8(), (int) text.getNumBytesAsUTF8(), "plain XML text (no header)");
    }

    // 3. Same root tag with an unknown version attribute: the code accepts it.
    if (xml != nullptr)
    {
        auto v2 = std::make_unique<juce::XmlElement> (*xml);
        v2->setAttribute ("stateVersion", 999);
        juce::MemoryBlock vb;
        juce::AudioProcessor::copyXmlToBinary (*v2, vb);
        p.setStateInformation (nullptr, 0);
        p.setStateInformation (vb.getData(), (int) vb.getSize());
        const bool v = p.clapLastStateLoadAccepted();
        std::printf ("  future-version: verdict=%d\n", (int) v);
        check (v && sameValues (origin, snapshot (p)), "same-tag state with unknown version accepted, values kept");
    }
    else
        check (false, "valid state parses as XML");

    std::printf ("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
