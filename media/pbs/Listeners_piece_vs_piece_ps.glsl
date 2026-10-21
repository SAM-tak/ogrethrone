// Pieces several of the editor's HlmsListeners fill, so each inserts its own: the pass buffer
// fields in the order HlmsListenerChain writes them (the frame's noise, motion vectors, then the
// Adaptive Irradiance Field) and the extra pass textures. Each is empty when its listener is idle
// in this pass.
@piece( custom_passBuffer )
	@insertpiece( framePassBuffer )
	@insertpiece( motionPassBuffer )
	@insertpiece( aifPassBuffer )
@end

@piece( custom_ps_uniformDeclaration )
	@insertpiece( ssaoTextures )
	@insertpiece( aifTextures )
@end
