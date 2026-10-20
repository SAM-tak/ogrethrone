// Screen-space motion vectors (TDDja/09), written by the depth prepass as its third target: the
// change in UV of the surface since the previous frame. MotionVectors (an HlmsListener) sets
// custom_motion_vectors in prepasses and writes the previous view-projection; ThronePbs writes
// each draw's previous world matrix into prevWorldBuf (read-only buffer slot 1).
@property( hlms_prepass && custom_motion_vectors )

@piece( motionPassBuffer )
	float4x4 prevViewProj;
@end

@piece( custom_VStoPS )
	INTERPOLANT( float3 motionCurrent, @counter(texcoord) );
	INTERPOLANT( float3 motionPrevious, @counter(texcoord) );
@end

@piece( custom_vs_uniformDeclaration )
	ReadOnlyBufferF( 1, float4, prevWorldBuf );
@end

@piece( custom_vs_posExecution )
	@property( !hlms_skeleton && !hlms_pose && !hlms_particle_system )
		float4 prevWorldPos = float4( mul( inVs_vertex, UNPACK_MAT4x3( prevWorldBuf, inVs_drawId ) ).xyz, 1.0 );
	@else
		float4 prevWorldPos = worldPos;
	@end
	// Clip-space x, y and w now and in the previous frame; divided per pixel.
	outVs.motionCurrent = outVs_Position.xyw;
	outVs.motionPrevious = mul( prevWorldPos, passBuf.prevViewProj ).xyw;
@end

@piece( custom_ps_output_types )
	layout(location = @counter(rtv_target)) out vec2 outMotion;
@end

@piece( custom_ps_posExecution )
	outMotion = ( inPs.motionCurrent.xy / inPs.motionCurrent.z -
				  inPs.motionPrevious.xy / inPs.motionPrevious.z ) * 0.5;
@end

@end
