package dev.raynium.client.validation;

import java.util.List;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.model.Model;
import net.minecraft.client.model.geom.ModelLayers;
import net.minecraft.client.model.object.banner.BannerFlagModel;
import net.minecraft.client.model.object.book.BookModel;
import net.minecraft.client.renderer.rendertype.RenderTypes;
import net.minecraft.core.component.DataComponents;
import net.minecraft.core.registries.Registries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.util.profiling.ResultField;
import net.minecraft.world.item.DyeColor;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.block.entity.BannerPatternLayers;
import net.minecraft.world.level.block.entity.BannerPatterns;

/** Deterministic inputs to the original GUI extractors, shared by both validation renderers. */
final class ValidationGalleryScreen extends Screen {
    private final Model.Simple skin;
    private final BookModel book;
    private final BannerFlagModel banner;
    private final BannerPatternLayers patterns;
    private final List<ItemStack> items;
    private final float phase;

    ValidationGalleryScreen(float phase) {
        super(Component.literal("GUI validation gallery"));
        if(!Float.isFinite(phase) || phase<0 || phase>1) throw new IllegalArgumentException("Gallery phase must be within 0..1");
        this.phase=phase;
        skin=new Model.Simple(minecraft.getEntityModels().bakeLayer(ModelLayers.PLAYER),RenderTypes::entityTranslucent);
        book=new BookModel(minecraft.getEntityModels().bakeLayer(ModelLayers.BOOK));
        banner=new BannerFlagModel(minecraft.getEntityModels().bakeLayer(ModelLayers.STANDING_BANNER_FLAG));
        var registry=minecraft.level.registryAccess().lookupOrThrow(Registries.BANNER_PATTERN);
        patterns=new BannerPatternLayers.Builder().add(registry.getOrThrow(BannerPatterns.CROSS),DyeColor.RED)
                .add(registry.getOrThrow(BannerPatterns.BORDER),DyeColor.BLUE).build();
        items=List.of(new ItemStack(Items.DIAMOND,32),new ItemStack(Items.COMPASS),new ItemStack(Items.CLOCK),
                new ItemStack(Items.DIAMOND_SWORD),new ItemStack(Items.SHIELD),new ItemStack(Items.TRIDENT),
                new ItemStack(Items.CHEST),new ItemStack(Items.BANNER.pick(DyeColor.WHITE)));
        for(int i=1;i<items.size();i++) items.get(i).set(DataComponents.ENCHANTMENT_GLINT_OVERRIDE,true);
        items.get(3).setDamageValue(900);
    }

    @Override public void extractBackground(GuiGraphicsExtractor graphics,int mouseX,int mouseY,float partialTick) {
        graphics.fill(0,0,width,height,0xff18212c);
    }

    @Override public void extractRenderState(GuiGraphicsExtractor graphics,int mouseX,int mouseY,float partialTick) {
        graphics.text(font,"Original GUI extractors / Unicode: 世界",16,12,0xffffffff);
        graphics.text(font,"Skin / book / banner / triangle fan",16,32,0xffc0d8ff);
        graphics.skin(skin,Identifier.withDefaultNamespace("textures/entity/player/wide/steve.png"),56,-5,30+phase*75,-1.0625f,16,55,116,185);
        graphics.book(book,Identifier.withDefaultNamespace("textures/entity/enchantment/enchanting_table_book.png"),60,.2f+phase*.8f,phase,136,70,246,150);
        graphics.bannerPattern(banner,DyeColor.WHITE,patterns,275,80,295,120);
        graphics.bannerPattern(banner,DyeColor.YELLOW,BannerPatternLayers.EMPTY,315,80,335,120);
        graphics.profilerChart(List.of(new ResultField("geometry",40,40,1),new ResultField("lighting",35,35,1),
                new ResultField("composition",25,25,1)),370,60,610,190);
        graphics.text(font,"Items / oversized model / glint / damage / count",16,202,0xffc0d8ff);
        for(int i=0;i<items.size();i++) {
            int x=32+i*70;
            graphics.item(items.get(i),x,240);
            graphics.itemDecorations(font,items.get(i),x,240);
        }
        graphics.enableScissor(20,292,330,322);
        graphics.text(font,"Clipped text: ABC xyz 0123456789 + inversion",12,302,0xffe8c080);
        graphics.textHighlight(90,300,210,314,true);
        graphics.disableScissor();
        graphics.text(font,"Stable glyphs and cached offscreen targets",355,304,0xffffffff,false);
    }

    @Override public boolean isPauseScreen() { return false; }
}
