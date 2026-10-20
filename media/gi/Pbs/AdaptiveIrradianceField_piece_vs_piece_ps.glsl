// Adaptive Irradiance Field (TDDja/12 5): the diffuse light of the bake, read from an octree of cells
// whose corners hold irradiance as L1 spherical harmonics. Bound by GlobalIllumination's
// HlmsListener, which sets custom_diffuse_gi, the texture slots and the pass buffer data.
@property( !hlms_shadowcaster && custom_diffuse_gi )

@piece( aifPassBuffer )
	// View space to finest cells from the volume's lower corner, as three rows.
	vec4 aifViewToGrid[3];
	// View space to world space directions, as three rows.
	vec4 aifViewToWorld[3];
	// xyz: top cells along each axis; w: a top cell's edge in finest cells.
	vec4 aifTop;
@end

@piece( custom_ps_uniformDeclaration )
	vulkan_layout( ogre_t@value(aifTop) ) uniform utexture3D aifTopTex;
	vulkan_layout( ogre_t@value(aifNodes) ) uniform utexture2D aifNodes;
	vulkan_layout( ogre_t@value(aifCells) ) uniform utexture2D aifCells;
	vulkan_layout( ogre_t@value(aifProbes) ) uniform texture2D aifProbes;
	vulkan( layout( ogre_s@value(aifTop) ) uniform sampler aifSampler );
@end

@piece( custom_ps_diffuseGi )
	// The share of the diffuse light the probes give; the ambient spherical harmonics give the
	// rest (DoAmbientLighting). 1 inside the volume, fading out over a finest cell beyond it.
	float ifdWeight = 0.0;
	{
		vec3 aifGrid = vec3( dot( passBuf.aifViewToGrid[0].xyz, inPs.pos.xyz ) + passBuf.aifViewToGrid[0].w,
							dot( passBuf.aifViewToGrid[1].xyz, inPs.pos.xyz ) + passBuf.aifViewToGrid[1].w,
							dot( passBuf.aifViewToGrid[2].xyz, inPs.pos.xyz ) + passBuf.aifViewToGrid[2].w );
		vec3 aifExtent = passBuf.aifTop.xyz * passBuf.aifTop.w;
		vec3 aifOutside = max( max( -aifGrid, aifGrid - aifExtent ), vec3( 0.0 ) );
		float aifWeight = clamp( 1.0 - length( aifOutside ), 0.0, 1.0 );
		aifGrid = clamp( aifGrid, vec3( 0.0 ), aifExtent - 0.001 );
		ivec3 aifTopCell = ivec3( aifGrid / passBuf.aifTop.w );
		uint aifEntry = texelFetch( usampler3D( aifTopTex, aifSampler ), aifTopCell, 0 ).x;
		if( aifEntry != 0xFFFFFFFFu && aifWeight > 0.0 )
		{
			// Down the octree to the leaf holding the point.
			vec3 aifCorner = vec3( aifTopCell ) * passBuf.aifTop.w;
			float aifSize = passBuf.aifTop.w;
			for( int aifLevel = 0; aifLevel < 6 && ( aifEntry & 0x80000000u ) == 0u; ++aifLevel )
			{
				aifSize *= 0.5;
				ivec3 aifChild = ivec3( greaterThanEqual( aifGrid, aifCorner + aifSize ) );
				aifCorner += vec3( aifChild ) * aifSize;
				uint aifNode = aifEntry + uint( aifChild.x + aifChild.y * 2 + aifChild.z * 4 );
				aifEntry = texelFetch( usampler2D( aifNodes, aifSampler ), ivec2( aifNode & 4095u, aifNode >> 12u ), 0 ).x;
			}
			uint aifCell = ( aifEntry & 0x7FFFFFFFu ) * 2u;
			uvec4 aifCorners0 = texelFetch( usampler2D( aifCells, aifSampler ), ivec2( aifCell & 4095u, aifCell >> 12u ), 0 );
			uvec4 aifCorners1 = texelFetch( usampler2D( aifCells, aifSampler ), ivec2( ( aifCell + 1u ) & 4095u, ( aifCell + 1u ) >> 12u ), 0 );
			vec3 aifT = clamp( ( aifGrid - aifCorner ) / aifSize, vec3( 0.0 ), vec3( 1.0 ) );
			vec3 aifNormal = normalize( vec3( dot( passBuf.aifViewToWorld[0].xyz, vec3( pixelData.normal ) ),
											 dot( passBuf.aifViewToWorld[1].xyz, vec3( pixelData.normal ) ),
											 dot( passBuf.aifViewToWorld[2].xyz, vec3( pixelData.normal ) ) ) );
			// Trilinear over the eight corners; each probe's light along the normal is a + b.n.
			vec3 aifLight = vec3( 0.0 );
			for( int i = 0; i < 8; ++i )
			{
				uint aifProbe = ( i < 4 ? aifCorners0[i] : aifCorners1[i - 4] ) * 3u;
				vec3 aifW3 = mix( vec3( 1.0 ) - aifT, aifT, vec3( ivec3( i, i >> 1, i >> 2 ) & ivec3( 1 ) ) );
				vec4 aifR = texelFetch( sampler2D( aifProbes, aifSampler ), ivec2( aifProbe & 4095u, aifProbe >> 12u ), 0 );
				vec4 aifG = texelFetch( sampler2D( aifProbes, aifSampler ), ivec2( ( aifProbe + 1u ) & 4095u, ( aifProbe + 1u ) >> 12u ), 0 );
				vec4 aifB = texelFetch( sampler2D( aifProbes, aifSampler ), ivec2( ( aifProbe + 2u ) & 4095u, ( aifProbe + 2u ) >> 12u ), 0 );
				aifLight += ( aifW3.x * aifW3.y * aifW3.z ) *
						   vec3( aifR.x + dot( aifR.yzw, aifNormal ), aifG.x + dot( aifG.yzw, aifNormal ), aifB.x + dot( aifB.yzw, aifNormal ) );
			}
			pixelData.envColourD += midf3_c( max( aifLight, vec3( 0.0 ) ) * aifWeight );
			ifdWeight = aifWeight;
		}
	}
@end

@end
