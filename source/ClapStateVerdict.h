#pragma once
// Lets the CLAP wrapper ask whether the last setStateInformation accepted the chunk,
// so stateLoad can return false for a well-formed XML state that is not ours.
// Keep this definition identical to the one added by
// cmake/patches/clap-juce-extensions-state.patch.
struct clap_juce_state_load_verdict
{
    virtual ~clap_juce_state_load_verdict() = default;
    virtual bool clapLastStateLoadAccepted() const = 0;
};
