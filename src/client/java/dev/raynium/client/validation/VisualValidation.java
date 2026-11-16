package dev.raynium.client.validation;

import com.google.gson.*;
import dev.raynium.client.RenderMode;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Screenshot;
import net.minecraft.client.gui.screens.PauseScreen;
import net.minecraft.client.gui.screens.inventory.InventoryScreen;
import net.minecraft.world.phys.Vec3;

/** Explicit test-driver entry shared by the vanilla reference and native takeover. */
public final class VisualValidation {
    private static VisualValidation active;
    private final Path directory;
    private final JsonArray scenes,results=new JsonArray();
    private final long started=System.nanoTime();
    private long stepStarted=started;
    private int index,frames;
    private String phase="loading";
    private CompletableFuture<Void> work;
    private JsonObject scene;
    private boolean particlesPrepared,buildingParticles,texturesPrepared;
    private long particleSeed;
    private final java.util.ArrayList<Path> captureFiles=new java.util.ArrayList<>();
    private final java.util.Set<com.mojang.renderpearl.api.pipeline.RenderPipeline> selectedPipelines=
        java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>());
    private final java.util.Set<String> pictureInPictureTypes=new java.util.TreeSet<>();
    private final dev.raynium.client.takeover.scene.VanillaPipelineCatalog catalog=dev.raynium.client.takeover.scene.VanillaPipelineCatalog.create();
    private VisualValidation(Path config) throws Exception {
        directory=config.toAbsolutePath().getParent();
        var root=JsonParser.parseString(Files.readString(config)).getAsJsonObject(); scenes=root.getAsJsonArray("scenes");
        if(scenes==null || scenes.isEmpty()) throw new IllegalArgumentException("Visual validation needs scenes");
        var names=new java.util.HashSet<String>();
        for(var entry:scenes) {
            var item=entry.getAsJsonObject(); var name=item.get("name").getAsString();
            if(root.has("defaults")) for(var setting:root.getAsJsonObject("defaults").entrySet())
                if(!item.has(setting.getKey())) item.add(setting.getKey(),setting.getValue().deepCopy());
            if(!name.matches("[a-zA-Z0-9_-]+") || !names.add(name)) throw new IllegalArgumentException("Invalid or duplicate scene name: "+name);
        }
    }
    public static void register() {
        String path=System.getProperty("raynium.validation.config"); if(path==null) return;
        if(System.getProperty("raynium.benchmark.config")!=null) throw new IllegalArgumentException("Visual validation and performance routes must run separately");
        try { active=new VisualValidation(Path.of(path)); }
        catch(Exception failure) { throw new IllegalStateException("Cannot load visual validation config",failure); }
    }
    public static void beforeFrame(Minecraft client) { if(active!=null) active.update(client); }
    public static void afterFrame(Minecraft client) { if(active!=null) active.capture(client); }
    public static boolean controlsScene() { return active!=null && active.scene!=null && !active.phase.equals("finished"); }
    public static long animationMillis(long original) {
        return controlsScene() && active.scene.has("animationMillis")?active.scene.get("animationMillis").getAsLong():original;
    }
    public static long spritePhase(java.util.List<net.minecraft.client.renderer.texture.SpriteContents.FrameInfo> frames) {
        if(!controlsScene() || !active.scene.has("animationMillis")) return -1;
        long duration=0; for(var frame:frames) duration+=frame.time();
        long tick=Math.floorMod(active.scene.get("animationMillis").getAsLong()/50,duration);
        int frame=0; while(tick>=frames.get(frame).time()) tick-=frames.get(frame++).time();
        return ((long)frame<<32)|tick;
    }
    public static float partialTick(float original) {
        return controlsScene() && active.scene.has("partialTick")?active.scene.get("partialTick").getAsFloat():original;
    }
    public static net.minecraft.resources.Identifier postEffect() {
        return controlsScene() && active.scene.has("postEffect")?net.minecraft.resources.Identifier.withDefaultNamespace(active.scene.get("postEffect").getAsString()):null;
    }
    public static boolean controlsParticles() { return controlsScene() && active.scene.has("particles"); }
    public static boolean buildingParticles() { return active!=null && active.buildingParticles; }
    public static long nextParticleSeed() { return active.particleSeed++; }
    public static void pipeline(com.mojang.renderpearl.api.pipeline.RenderPipeline pipeline) {
        if(controlsScene()) active.selectedPipelines.add(pipeline);
    }
    public static void pictureInPicture(net.minecraft.client.renderer.state.gui.pip.PictureInPictureRenderState state) {
        if(controlsScene()) active.pictureInPictureTypes.add(state.getClass().getSimpleName());
    }
    private JsonArray selectedPipelines() {
        var result=new JsonArray();
        for(var entry:catalog.entries()) if(selectedPipelines.contains(entry.pipeline())) {
            var item=new JsonObject(); item.addProperty("id",entry.id()); item.add("names",new Gson().toJsonTree(entry.names())); result.add(item);
        }
        return result;
    }
    private void prepareParticles(Minecraft client) {
        if(!controlsParticles() || particlesPrepared || !work.isDone() || !phase.equals("settling")) return;
        double seconds=scene.has("settleSeconds")?scene.get("settleSeconds").getAsDouble():3;
        if((System.nanoTime()-stepStarted)/1e9<seconds || !client.levelRenderer.hasRenderedAllSections()) return;
        client.particleEngine.clearParticles();
        particleSeed=scene.has("particleSeed")?scene.get("particleSeed").getAsLong():1234567;
        client.particleEngine.getRandom().setSeed(particleSeed);
        dev.raynium.client.mixin.VisualValidationSpellAccess.raynium$random().setSeed(particleSeed);
        buildingParticles=true;
        try {
            var ops=client.level.registryAccess().createSerializationContext(com.mojang.serialization.JsonOps.INSTANCE);
            for(var entry:scene.getAsJsonArray("particles")) {
                var item=entry.getAsJsonObject();
                var options=net.minecraft.core.particles.ParticleTypes.CODEC.parse(ops,item.get("type")).getOrThrow();
                int count=item.has("count")?item.get("count").getAsInt():1;
                if(count<1 || count>4096) throw new IllegalArgumentException("Particle fixture count outside 1..4096");
                for(int i=0;i<count;i++) {
                    var particle=client.particleEngine.createParticle(options,number(item,"x"),number(item,"y"),number(item,"z"),
                        item.has("dx")?number(item,"dx"):0,item.has("dy")?number(item,"dy"):0,item.has("dz")?number(item,"dz"):0);
                    if(particle==null) throw new IllegalArgumentException("Particle fixture has no provider: "+item.get("type"));
                }
            }
            client.particleEngine.tick(); // Flush the original engine's pending additions before advancing particle age.
            int ticks=scene.has("particleTicks")?scene.get("particleTicks").getAsInt():0;
            if(ticks<0 || ticks>1200) throw new IllegalArgumentException("Particle fixture age outside 0..1200");
            for(int i=0;i<ticks;i++) client.particleEngine.tick();
            particlesPrepared=true;
        } finally { buildingParticles=false; }
    }
    private void update(Minecraft client) {
        if(phase.equals("finished")) return;
        try {
            if((System.nanoTime()-stepStarted)/1e9>300) throw new IllegalStateException("Scene timed out in "+phase);
            if(client.player==null || client.level==null || client.gui.overlay()!=null || !client.player.connection.hasClientLoaded()) return;
            if(phase.equals("loading")) {
                client.options.pauseOnLostFocus=false; client.gui.setScreen(null);
                if(index==scenes.size()) { finish(client,null); return; }
                scene=scenes.get(index).getAsJsonObject(); stepStarted=System.nanoTime(); frames=0; particlesPrepared=false; texturesPrepared=false; selectedPipelines.clear(); pictureInPictureTypes.clear();
                org.slf4j.LoggerFactory.getLogger("raynium.validation").info("Preparing visual scene {}",scene.get("name").getAsString());
                if(scene.has("oit")) client.options.improvedTransparency().set(scene.get("oit").getAsBoolean());
                client.options.cloudStatus().set(net.minecraft.client.CloudStatus.valueOf(
                    scene.has("clouds")?scene.get("clouds").getAsString().toUpperCase(java.util.Locale.ROOT):"FANCY"));
                if(scene.has("wireframe")) {
                    boolean wireframe=scene.get("wireframe").getAsBoolean();
                    if(wireframe && !com.mojang.blaze3d.systems.RenderSystem.isWireframeAvailable()) throw new IllegalStateException("Wireframe fixture requires device support");
                    client.wireframe=wireframe;
                }
                boolean hide=scene.has("hideHud") && scene.get("hideHud").getAsBoolean();
                if(client.gui.hud.isHidden()!=hide) client.gui.hud.toggle();
                client.player.input=new net.minecraft.client.player.ClientInput();
                client.player.stopUsingItem();
                client.player.itemActivation().reset();
                for(String entry:java.util.List.of("entity_hitboxes","chunk_borders","3d_crosshair"))
                    client.debugEntries.setStatus(net.minecraft.resources.Identifier.withDefaultNamespace(entry),
                        net.minecraft.client.gui.components.debug.DebugScreenEntryStatus.NEVER);
                if(scene.has("debugEntries")) for(var entry:scene.getAsJsonArray("debugEntries"))
                    client.debugEntries.setStatus(net.minecraft.resources.Identifier.parse(entry.getAsString()),
                        net.minecraft.client.gui.components.debug.DebugScreenEntryStatus.ALWAYS_ON);
                client.level.destroyBlockProgress(Integer.MAX_VALUE,net.minecraft.core.BlockPos.ZERO,-1);
                var server=client.getSingleplayerServer();
                if(server==null) throw new IllegalStateException("Visual validation requires a copied local world");
                var playerId=client.player.getUUID();
                work=server.submit(()->{
                    var player=server.getPlayerList().getPlayer(playerId);
                    var source=server.createCommandSourceStack().withEntity(player).withLevel(player.level()).withPosition(player.position()).withSuppressedOutput();
                    if(scene.has("commands")) for(var command:scene.getAsJsonArray("commands")) {
                        var failure=new java.util.concurrent.atomic.AtomicBoolean();
                        server.getCommands().performPrefixedCommand(source.withCallback((ok,result)->{if(!ok) failure.set(true);}),command.getAsString());
                        if(failure.get()) throw new IllegalArgumentException("Scene command failed: "+command.getAsString());
                    }
                    if(scene.has("visualLightning") && scene.get("visualLightning").getAsBoolean())
                        for(var entity:player.level().getAllEntities()) if(entity instanceof net.minecraft.world.entity.LightningBolt bolt) bolt.setVisualOnly(true);
                    if(scene.has("mapPattern") && scene.get("mapPattern").getAsBoolean()) {
                        var map=net.minecraft.world.item.MapItem.create(player.level(),0,0,(byte)0,false,false);
                        var id=map.get(net.minecraft.core.component.DataComponents.MAP_ID);
                        var data=player.level().getMapData(id);
                        if(data==null) throw new IllegalStateException("Map fixture has no saved data");
                        for(int y=0;y<128;y++) for(int x=0;x<128;x++) data.setColor(x,y,(byte)((1+(x/16)+(y/16)*8)%61*4+2));
                        data=data.locked(); player.level().setMapData(id,data);
                        player.setItemSlot(net.minecraft.world.entity.EquipmentSlot.MAINHAND,map);
                        data.tickCarriedBy(player,map,null);
                        var packet=data.getUpdatePacket(id,player);
                        if(packet==null) throw new IllegalStateException("Map fixture has no update packet");
                        player.connection.send(packet);
                    }
                    if(scene.has("gameTime")) server.getWorldData().overworldData().setGameTime(scene.get("gameTime").getAsLong());
                    if(scene.has("camera")) {
                        var camera=scene.getAsJsonObject("camera");
                        if(!player.teleportTo(player.level(),number(camera,"x"),number(camera,"y"),number(camera,"z"),Set.of(),
                            (float)number(camera,"yaw"),(float)number(camera,"pitch"),true)) throw new IllegalStateException("Scene teleport failed");
                    }
                });
                phase="commands";
            }
            if(phase.equals("commands") && work.isDone()) {
                work.join();
                String screen=scene.has("screen")?scene.get("screen").getAsString():"none";
                switch(screen) {
                    case "none" -> client.gui.setScreen(null);
                    case "inventory" -> client.gui.setScreen(new InventoryScreen(client.player));
                    case "pause" -> client.gui.setScreen(new PauseScreen(true));
                    case "gallery" -> client.gui.setScreen(new ValidationGalleryScreen(scene.has("galleryPhase")?scene.get("galleryPhase").getAsFloat():0));
                    case "title" -> {
                        var title=new net.minecraft.client.gui.screens.TitleScreen(); client.gui.setScreen(title);
                        ((dev.raynium.client.mixin.VisualValidationTitleAccess)title).raynium$splash(null);
                        client.gameRenderer.panorama().holdSpin();
                        ((dev.raynium.client.mixin.VisualValidationPanoramaAccess)client.gameRenderer.panorama()).raynium$spin(20);
                    }
                    default -> throw new IllegalArgumentException("Unknown validation screen: "+screen);
                }
                if(scene.has("reload") && scene.get("reload").getAsBoolean()) work=client.reloadResourcePacks();
                phase="settling"; stepStarted=System.nanoTime();
            }
            if(scene.has("camera") && !phase.equals("commands")) {
                var camera=scene.getAsJsonObject("camera");
                client.player.setDeltaMovement(Vec3.ZERO);
                client.player.absSnapTo(number(camera,"x"),number(camera,"y"),number(camera,"z"),(float)number(camera,"yaw"),(float)number(camera,"pitch"));
            }
            if(!phase.equals("commands")) {
                if(scene.has("clearNotifications") && scene.get("clearNotifications").getAsBoolean()) {
                    client.gui.hud.getChat().clearMessages(true);
                    client.gui.toastManager().clear();
                }
                if(scene.has("useItemTicks")) {
                    var hand=scene.has("useItemHand") && scene.get("useItemHand").getAsString().equals("offhand")?
                        net.minecraft.world.InteractionHand.OFF_HAND:net.minecraft.world.InteractionHand.MAIN_HAND;
                    int elapsed=scene.get("useItemTicks").getAsInt(),duration=client.player.getItemInHand(hand).getUseDuration(client.player);
                    if(duration>0) {
                        if(elapsed<0 || elapsed>=duration) throw new IllegalArgumentException("Invalid item use fixture age");
                        client.player.startUsingItem(hand);
                        ((dev.raynium.client.mixin.VisualValidationUseItemAccess)client.player).raynium$useItemRemaining(duration-elapsed);
                    } else if((System.nanoTime()-stepStarted)/1e9>2) throw new IllegalArgumentException("Item use fixture has no usable item");
                }
                if(scene.has("activationTicks")) {
                    int elapsed=scene.get("activationTicks").getAsInt();
                    if(elapsed<0 || elapsed>=40) throw new IllegalArgumentException("Activation fixture age outside 0..39");
                    client.player.itemActivation().activate(new net.minecraft.world.item.ItemStack(net.minecraft.world.item.Items.TOTEM_OF_UNDYING),
                        net.minecraft.util.RandomSource.create(149521));
                    for(int i=0;i<elapsed;i++) client.player.itemActivation().tick();
                }
                if(scene.has("breakingBlock")) {
                    var item=scene.getAsJsonObject("breakingBlock"); var pos=item.getAsJsonArray("position");
                    client.level.destroyBlockProgress(Integer.MAX_VALUE,new net.minecraft.core.BlockPos(pos.get(0).getAsInt(),pos.get(1).getAsInt(),pos.get(2).getAsInt()),item.get("stage").getAsInt());
                }
                if(scene.has("vignetteBrightness")) {
                    double brightness=number(scene,"vignetteBrightness");
                    if(brightness<0 || brightness>1) throw new IllegalArgumentException("Vignette brightness outside 0..1");
                    client.gui.hud.vignetteBrightness=(float)brightness;
                }
                if(scene.has("gameTime")) {
                    long time=scene.get("gameTime").getAsLong(); client.level.setTimeFromServer(time);
                    if(client.level.endFlashState()!=null) client.level.endFlashState().tick(time);
                }
                if(scene.has("animationMillis")) for(var entity:client.level.entitiesForRendering()) {
                    entity.tickCount=(int)(scene.get("animationMillis").getAsLong()/50);
                    if(entity instanceof net.minecraft.world.entity.boss.enderdragon.EndCrystal crystal) crystal.time=entity.tickCount;
                    if(entity instanceof net.minecraft.world.entity.LightningBolt bolt) bolt.seed=149521;
                    if(scene.has("dragonDeathTicks") && entity instanceof net.minecraft.world.entity.boss.enderdragon.EnderDragon dragon)
                        dragon.dragonDeathTime=scene.get("dragonDeathTicks").getAsInt();
                    if(scene.has("entityHurtTicks") && entity instanceof net.minecraft.world.entity.LivingEntity living)
                        living.hurtTime=scene.get("entityHurtTicks").getAsInt();
                }
                if(scene.has("rain")) client.level.setRainLevel((float)number(scene,"rain"));
                if(scene.has("thunder")) client.level.setThunderLevel((float)number(scene,"thunder"));
                if(scene.has("blockAnimations")) for(var entry:scene.getAsJsonArray("blockAnimations")) {
                    var item=entry.getAsJsonObject(); var pos=item.getAsJsonArray("position");
                    var block=client.level.getBlockEntity(new net.minecraft.core.BlockPos(pos.get(0).getAsInt(),pos.get(1).getAsInt(),pos.get(2).getAsInt()));
                    if(block==null) continue;
                    if(block instanceof net.minecraft.world.level.block.entity.EnchantingTableBlockEntity table) {
                        table.time=item.get("time").getAsInt();
                        table.flip=table.oFlip=(float)number(item,"flip");
                        table.open=table.oOpen=(float)number(item,"open");
                        table.rot=table.oRot=(float)number(item,"rotation");
                    } else if(block instanceof net.minecraft.world.level.block.entity.TheEndGatewayBlockEntity gateway) {
                        var access=(dev.raynium.client.mixin.VisualValidationGatewayAccess)gateway;
                        access.raynium$age(item.get("age").getAsLong()); access.raynium$cooldown(item.has("cooldown")?item.get("cooldown").getAsInt():0);
                    } else throw new IllegalArgumentException("Unknown block animation fixture: "+block.getClass());
                }
                if(!texturesPrepared && phase.equals("settling") && work.isDone()) {
                    work.join();
                    // Frozen worlds do not tick textures; apply the explicit fixture time after reload completes.
                    if(scene.has("animationMillis")) client.getTextureManager().tick();
                    texturesPrepared=true;
                }
                prepareParticles(client);
            }
            if(phase.equals("capturing") && work.isDone() && exportsComplete()) {
                work.join(); ++index; phase="loading";
            }
        } catch(Exception failure) { finish(client,failure); }
    }
    private static double number(JsonObject object,String key) {
        double value=object.get(key).getAsDouble(); if(!Double.isFinite(value)) throw new IllegalArgumentException("Non-finite camera field: "+key); return value;
    }
    private boolean exportsComplete() throws java.io.IOException {
        var tail=java.nio.ByteBuffer.allocate(8);
        for(var file:captureFiles) {
            if(!Files.isRegularFile(file)) return false;
            try(var channel=java.nio.channels.FileChannel.open(file)) {
                long size=channel.size(); if(size<12) return false;
                tail.clear(); if(channel.read(tail,size-8)!=8 || tail.getLong(0)!=0x49454e44ae426082L) return false;
            }
        }
        return true;
    }
    private void export(Minecraft client,JsonObject result,String name) throws java.io.IOException {
        captureFiles.clear();
        Path exports=directory.resolve(name+"-exports");
        if(scene.has("panorama") && scene.get("panorama").getAsBoolean()) {
            Files.createDirectories(exports);
            client.grabPanoramixScreenshot(exports.toFile());
            for(int i=0;i<6;i++) captureFiles.add(exports.resolve("screenshots/panorama_"+i+".png"));
        }
        if(scene.has("textureExports")) for(var entry:scene.getAsJsonArray("textureExports")) {
            var id=net.minecraft.resources.Identifier.parse(entry.getAsString());
            var texture=client.getTextureManager().getTexture(id);
            if(!(texture instanceof net.minecraft.client.renderer.texture.Dumpable dumpable)) throw new IllegalArgumentException("Texture is not exportable: "+id);
            Files.createDirectories(exports);
            dumpable.dumpContents(id,exports);
            if(texture instanceof net.minecraft.client.renderer.texture.TextureAtlas) {
                for(int mip=0;mip<texture.getTexture().getMipLevels();mip++) captureFiles.add(exports.resolve(id.toDebugFileName()+"_"+mip+".png"));
            } else if(texture instanceof net.minecraft.client.renderer.texture.DynamicTexture) captureFiles.add(exports.resolve(id.toDebugFileName()+".png"));
            else throw new IllegalArgumentException("Unknown texture export file naming: "+texture.getClass());
        }
        var files=new JsonArray(); for(var file:captureFiles) files.add(directory.relativize(file).toString().replace('\\','/'));
        result.add("exports",files);
    }
    private void capture(Minecraft client) {
        if(!phase.equals("settling") || client.gui.overlay()!=null || !work.isDone() || !texturesPrepared) return;
        try {
            work.join(); ++frames;
            int required=scene.has("settleFrames")?scene.get("settleFrames").getAsInt():120;
            double seconds=scene.has("settleSeconds")?scene.get("settleSeconds").getAsDouble():3;
            if(frames<required || (System.nanoTime()-stepStarted)/1e9<seconds || !client.levelRenderer.hasRenderedAllSections()) return;
            if(controlsParticles() && !particlesPrepared) return;
            var entities=new java.util.TreeMap<String,Integer>();
            for(var entity:client.level.entitiesForRendering()) entities.merge(net.minecraft.core.registries.BuiltInRegistries.ENTITY_TYPE.getKey(entity.getType()).toString(),1,Integer::sum);
            if(scene.has("requiredEntities")) for(var requiredEntity:scene.getAsJsonArray("requiredEntities"))
                if(!entities.containsKey(requiredEntity.getAsString())) throw new IllegalStateException("Missing visual fixture entity: "+requiredEntity.getAsString());
            if(scene.has("requiredPictureInPicture")) for(var requiredType:scene.getAsJsonArray("requiredPictureInPicture"))
                if(!pictureInPictureTypes.contains(requiredType.getAsString())) throw new IllegalStateException("Missing picture-in-picture fixture: "+requiredType.getAsString());
            var result=new JsonObject(); String name=scene.get("name").getAsString(); result.addProperty("name",name);
            result.add("entities",new Gson().toJsonTree(entities));
            result.addProperty("width",client.getWindow().getWidth()); result.addProperty("height",client.getWindow().getHeight());
            result.addProperty("oit",client.options.improvedTransparency().get()); results.add(result);
            result.addProperty("wireframe",client.wireframe); result.add("settings",scene.deepCopy());
            result.add("postEffects",new Gson().toJsonTree(client.gameRenderer.gameRenderState().requestedPostEffects.stream().map(Object::toString).toList()));
            result.addProperty("particles",client.particleEngine.countParticles());
            result.add("selectedVanillaPipelines",selectedPipelines());
            result.add("pictureInPictureTypes",new Gson().toJsonTree(pictureInPictureTypes));
            var light=client.gameRenderer.gameRenderState().lightmapRenderState;
            result.add("lightmap",new Gson().toJsonTree(new float[]{light.skyFactor,light.blockFactor,light.brightness,light.nightVisionEffectIntensity,light.darknessEffectScale,
                light.bossOverlayWorldDarkening,light.ambientColor.x(),light.ambientColor.y(),light.ambientColor.z(),light.skyLightColor.x(),light.skyLightColor.y(),light.skyLightColor.z(),
                light.blockLightTint.x(),light.blockLightTint.y(),light.blockLightTint.z()}));
            var completed=new CompletableFuture<Void>(); work=completed; phase="capturing";
            Screenshot.takeScreenshot(client.gameRenderer.mainRenderTarget(),image->net.minecraft.util.Util.ioPool().execute(()->{
                try(image) { image.writeToFile(directory.resolve(name+".png")); completed.complete(null); }
                catch(Exception failure) { completed.completeExceptionally(failure); }
            }));
            export(client,result,name);
        } catch(Exception failure) { finish(client,failure); }
    }
    private void finish(Minecraft client,Exception failure) {
        var result=new JsonObject(); result.addProperty("status",failure==null?"completed":"failed");
        result.addProperty("renderer",RenderMode.REFERENCE?"vanilla":"raynium"); result.addProperty("phase",phase); result.addProperty("index",index);
        result.addProperty("backend",RenderMode.REFERENCE?"vulkan":System.getProperty("raynium.rhi.backend","d3d12"));
        if(failure!=null) result.addProperty("error",failure.toString()); result.add("scenes",results); phase="finished";
        try { Files.writeString(directory.resolve("visual-result.json"),new GsonBuilder().setPrettyPrinting().create().toJson(result)); }
        catch(Exception error) { org.slf4j.LoggerFactory.getLogger("raynium.validation").error("Cannot write visual result",error); }
        if(failure!=null) org.slf4j.LoggerFactory.getLogger("raynium.validation").error("Visual validation failed",failure);
        client.stop();
    }
}
