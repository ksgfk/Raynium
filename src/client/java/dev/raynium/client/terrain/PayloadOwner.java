package dev.raynium.client.terrain;

public interface PayloadOwner {
    SectionPayload raynium$payload();
    void raynium$payload(SectionPayload payload);
    default int raynium$readyMask() { return 0; }
}
