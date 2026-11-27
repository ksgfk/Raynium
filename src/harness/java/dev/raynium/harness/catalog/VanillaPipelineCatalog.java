package dev.raynium.harness.catalog;

import com.mojang.renderpearl.api.pipeline.RenderPipeline;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Objects;
import net.minecraft.client.renderer.RenderPipelines;
import net.minecraft.client.renderer.oit.OitPipelineSet;
import net.minecraft.client.renderer.oit.OitStage;
/** Enumerates fields as well as registries: the vanilla registry can overwrite same-location variants. */
public final class VanillaPipelineCatalog {
    public record Entry(int id, List<String> names, RenderPipeline pipeline, PipelineIdentity identity, boolean optional) {
        public Entry {
            names = List.copyOf(names);
        }
    }
    private final IdentityHashMap<RenderPipeline, Entry> byPipeline;
    private final List<Entry> entries;
    private VanillaPipelineCatalog(IdentityHashMap<RenderPipeline, Entry> byPipeline, List<Entry> entries) {
        this.byPipeline = byPipeline;
        this.entries = List.copyOf(entries);
    }
    public static VanillaPipelineCatalog create() {
        var fields = RenderPipelines.class.getFields();
        var required = RenderPipelines.requiredPipelines();
        var optionalPipelines = RenderPipelines.optionalPipelines();
        var names = new IdentityHashMap<RenderPipeline, List<String>>(Math.max(fields.length, required.size() + optionalPipelines.size()));
        Arrays.sort(fields, Comparator.comparing(Field::getName));
        try {
            for (var field : fields) {
                if (!Modifier.isStatic(field.getModifiers()))
                    continue;
                if (field.getType() == RenderPipeline.class)
                    add(names, (RenderPipeline)field.get(null), field.getName());
                else if (field.getType() == OitPipelineSet.class) {
                    var set = (OitPipelineSet)field.get(null);
                    for (var stage : OitStage.values())
                        add(names, set.getPipeline(stage), field.getName() + "/" + stage.name());
                }
            }
        } catch (ReflectiveOperationException failure) {
            throw new IllegalStateException("Cannot enumerate vanilla pipelines", failure);
        }
        for (var pipeline : required)
            add(names, pipeline, "required:" + pipeline.getLocation());
        var optional = Collections.newSetFromMap(new IdentityHashMap<RenderPipeline, Boolean>(optionalPipelines.size()));
        for (var pipeline : optionalPipelines) {
            optional.add(pipeline);
            add(names, pipeline, "optional:" + pipeline.getLocation());
        }
        var ordered = new ArrayList<>(names.entrySet());
        ordered.sort(Comparator.comparing(entry -> entry.getValue().getFirst()));
        var entries = new ArrayList<Entry>(ordered.size());
        var byPipeline = new IdentityHashMap<RenderPipeline, Entry>(ordered.size());
        for (var item : ordered) {
            var pipeline = item.getKey();
            var entry = new Entry(entries.size() + 1, item.getValue(), pipeline, PipelineIdentity.of(pipeline), optional.contains(pipeline));
            entries.add(entry);
            byPipeline.put(pipeline, entry);
        }
        return new VanillaPipelineCatalog(byPipeline, entries);
    }
    private static void add(IdentityHashMap<RenderPipeline, List<String>> names, RenderPipeline pipeline, String name) {
        names.computeIfAbsent(Objects.requireNonNull(pipeline), ignored -> new ArrayList<>(2)).add(name);
    }
    public List<Entry> entries() {
        return entries;
    }
    public Entry require(RenderPipeline pipeline) {
        var entry = byPipeline.get(pipeline);
        if (entry == null)
            throw new IllegalArgumentException("Unregistered rendering pipeline: " + pipeline.getLocation());
        return entry;
    }
}
