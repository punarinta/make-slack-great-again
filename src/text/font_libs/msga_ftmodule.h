/* The FreeType modules msga's text layer reaches (FT_CONFIG_MODULES_H, see
 * ../font_libs.cmake): TrueType/OpenType and CFF outlines, the auto-hinter
 * behind FT_LOAD_TARGET_LIGHT, the anti-aliasing renderer. No Type 1, CID,
 * PFR, Type 42, Windows .fnt, PCF/BDF, OT-SVG or SDF, and no monochrome
 * renderer (msga never renders FT_RENDER_MODE_MONO). */
FT_USE_MODULE(FT_Module_Class, autofit_module_class)
FT_USE_MODULE(FT_Driver_ClassRec, tt_driver_class)
FT_USE_MODULE(FT_Driver_ClassRec, cff_driver_class)
FT_USE_MODULE(FT_Module_Class, psaux_module_class)
FT_USE_MODULE(FT_Module_Class, psnames_module_class)
FT_USE_MODULE(FT_Module_Class, pshinter_module_class)
FT_USE_MODULE(FT_Module_Class, sfnt_module_class)
FT_USE_MODULE(FT_Renderer_Class, ft_smooth_renderer_class)
