// Screen-space motion vectors (TDDja/09), written by the depth prepass as its third target: the
// change in UV of the surface since the previous frame. MotionVectors (an HlmsListener) sets
// custom_motion_vectors in prepasses and writes the previous view-projection.
@property( hlms_prepass && custom_motion_vectors )

@piece( motionPassBuffer )
	float4x4 prevViewProj;
@end

@piece( custom_VStoPS )
	INTERPOLANT( float3 motionCurrent, @counter(texcoord) );
	INTERPOLANT( float3 motionPrevious, @counter(texcoord) );
@end

@piece( custom_vs_posExecution )
	// Clip-space x, y and w now and in the previous frame; divided per pixel.
	outVs.motionCurrent = outVs_Position.xyw;
	outVs.motionPrevious = mul( worldPos, passBuf.prevViewProj ).xyw;
@end

@piece( custom_ps_output_types )
	layout(location = @counter(rtv_target)) out vec2 outMotion;
@end

@piece( custom_ps_posExecution )
	outMotion = ( inPs.motionCurrent.xy / inPs.motionCurrent.z -
				  inPs.motionPrevious.xy / inPs.motionPrevious.z ) * 0.5;
@end

@end
