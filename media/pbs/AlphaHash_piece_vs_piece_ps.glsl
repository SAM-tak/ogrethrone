// Alpha hashing that temporal anti-aliasing can average (TDDja/09): OGRE's hash is stable in the
// object's space, so a pixel keeps or drops a surface every frame alike. FrameNoise (an
// HlmsListener) hands each pass that is not a shadow caster this frame's offset, which turns the
// threshold (still uniformly distributed). The prepass and the passes after it use the same
// offset in a frame, so their depths agree; shadow casters keep the stable hash.
@property( !hlms_shadowcaster && custom_frame_noise )

@piece( framePassBuffer )
	// x: this frame's offset in [0, 1).
	vec4 frameNoise;
@end

@piece( custom_alphaHashThreshold )
	alphaThreshold = fract( alphaThreshold + passBuf.frameNoise.x );
@end

@end
