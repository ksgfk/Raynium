package dev.raynium.client.terrain;

public interface SectionStateAccess {
    record State(SectionPayload payload, int readyMask) {}
    State raynium$state();
    long raynium$captureGen();
    int raynium$captureSlot();
    void raynium$setCaptureSlot(long generation, int slot);
}
