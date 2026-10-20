// The pass buffer fields of the editor's HlmsListeners, in the order HlmsListenerChain writes
// them: motion vectors, then the Adaptive Irradiance Field. Each piece is empty when its
// listener writes nothing in this pass.
@piece( custom_passBuffer )
	@insertpiece( motionPassBuffer )
	@insertpiece( aifPassBuffer )
@end
