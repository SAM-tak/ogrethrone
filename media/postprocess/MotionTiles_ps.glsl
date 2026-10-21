#version ogre_glsl_ver_330

// The motion blur's tiles (McGuire et al. 2012): with 'neighbours' 0, the fastest motion in
// each tile of the velocities (one output texel per tile); with 1, the fastest of the 3x3 tiles
// around each tile, so that a pixel knows whether anything moving fast enough to reach it is near.

vulkan_layout( ogre_t0 ) uniform texture2D velocities;
vulkan( layout( ogre_s0 ) uniform sampler velocitySampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform float neighbours;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec2 fastest;

void main()
{
	ivec2 inputSize = textureSize( vkSampler2D( velocities, velocitySampler ), 0 );
	ivec2 tile = ivec2( gl_FragCoord.xy );
	fastest = vec2( 0.0 );
	float longest = -1.0;
	if( neighbours > 0.5 )
	{
		for( int y = -1; y <= 1; ++y )
		{
			for( int x = -1; x <= 1; ++x )
			{
				vec2 v = texelFetch( vkSampler2D( velocities, velocitySampler ),
									 clamp( tile + ivec2( x, y ), ivec2( 0 ), inputSize - 1 ), 0 ).xy;
				if( dot( v, v ) > longest )
				{
					longest = dot( v, v );
					fastest = v;
				}
			}
		}
		return;
	}
	// One tile of the full-size velocities per output texel.
	ivec2 start = tile * 16;
	for( int y = 0; y < 16; ++y )
	{
		for( int x = 0; x < 16; ++x )
		{
			vec2 v = texelFetch( vkSampler2D( velocities, velocitySampler ),
								 min( start + ivec2( x, y ), inputSize - 1 ), 0 ).xy;
			if( dot( v, v ) > longest )
			{
				longest = dot( v, v );
				fastest = v;
			}
		}
	}
}
