package dev.raynium.client.takeover.scene;

/** Releases borrowed CPU storage after synchronous native preparation, including failed/unsubmitted frames. */
public interface SceneInput { void release(); }
