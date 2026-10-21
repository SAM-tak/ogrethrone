// Screen-space motion vectors (TDDja/09), written by the depth prepass as its third target: the
// change in UV of the surface since the previous frame. MotionVectors (an HlmsListener) sets
// custom_motion_vectors in prepasses and writes the previous view-projection; ThronePbs writes
// each draw's previous world matrix (or a skinned draw's previous bone matrices) into
// prevWorldBuf (read-only buffer slot 1).
// The fourth target is the surface's subsurface scattering width in metres (0 for none), which
// a material's custom piece defines as THRONE_SUBSURFACE_WIDTH (see surfacePiece in Model.cpp).
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
	@property( hlms_skeleton )
		// The previous bone matrices start at the row the draw's first row names (ThronePbs);
		// negative when there was no room for them.
		float prevBones = readOnlyFetch( prevWorldBuf, int( inVs_drawId * 4u ) ).x;
		float4 prevWorldPos = worldPos;
		if( prevBones >= 0.0 )
		{
			prevWorldPos = float4( 0.0, 0.0, 0.0, 1.0 );
			@foreach( hlms_bones_per_vertex, n )
			{
				uint _p = uint( prevBones ) + inVs_blendIndices[@n] * 3u;
				ogre_float4x3 prevBone = makeOgreFloat4x3( readOnlyFetch( prevWorldBuf, int( _p ) ),
														   readOnlyFetch( prevWorldBuf, int( _p + 1u ) ),
														   readOnlyFetch( prevWorldBuf, int( _p + 2u ) ) );
				prevWorldPos.xyz += mul( inputPos, prevBone ) * inVs_blendWeights[@n];
			}
			@end
		}
	@else
		@property( !hlms_pose && !hlms_particle_system )
			float4 prevWorldPos = float4( mul( inVs_vertex, UNPACK_MAT4x3( prevWorldBuf, inVs_drawId ) ).xyz, 1.0 );
		@else
			float4 prevWorldPos = worldPos;
		@end
	@end
	// Clip-space x, y and w now and in the previous frame; divided per pixel.
	outVs.motionCurrent = outVs_Position.xyw;
	outVs.motionPrevious = mul( prevWorldPos, passBuf.prevViewProj ).xyw;
@end

@piece( custom_ps_output_types )
	layout(location = @counter(rtv_target)) out vec2 outMotion;
	layout(location = @counter(rtv_target)) out float outSubsurface;
@end

@piece( custom_ps_posExecution )
	outMotion = ( inPs.motionCurrent.xy / inPs.motionCurrent.z -
				  inPs.motionPrevious.xy / inPs.motionPrevious.z ) * 0.5;
#ifdef THRONE_SUBSURFACE_WIDTH
	outSubsurface = THRONE_SUBSURFACE_WIDTH;
#else
	outSubsurface = 0.0;
#endif
@end

@end
