// Screen-space ambient occlusion (TDDja/09): AmbientOcclusion (an HlmsListener) sets
// custom_ssao in the passes that use the depth prepass and binds the occlusion computed from it,
// which scales the indirect light only: the direct light stays as it is.
@property( !hlms_shadowcaster && custom_ssao )

@piece( ssaoTextures )
	vulkan_layout( ogre_t@value(ssaoTexture) ) uniform texture2D ssaoTexture;
@end

@piece( custom_ps_posAmbient )
	@property( needs_env_brdf )
		midf ambientOcclusion = midf_c( OGRE_Load2D( ssaoTexture, ivec2( gl_FragCoord.xy ), 0 ).x );
		pixelData.envColourD *= ambientOcclusion;
		pixelData.envColourS *= ambientOcclusion;
	@end
@end

@end
