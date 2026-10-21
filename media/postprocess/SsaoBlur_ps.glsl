#version ogre_glsl_ver_330

// Smooths the ambient occlusion's grain over 5x5 pixels, weighting each by how close its depth
// is to the centre's so that occlusion does not bleed across silhouettes.

vulkan_layout( ogre_t0 ) uniform texture2D occlusion;
vulkan( layout( ogre_s0 ) uniform sampler occlusionSampler );
vulkan_layout( ogre_t1 ) uniform texture2D depth;
vulkan( layout( ogre_s1 ) uniform sampler depthSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The projection's z row (A, B): the distance to a depth d is B / (d + A).
	uniform vec2 depthParams;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float smoothed;

float distanceAt( ivec2 at )
{
	return depthParams.y / ( texelFetch( vkSampler2D( depth, depthSampler ), at, 0 ).x + depthParams.x );
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( occlusion, occlusionSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	float centre = distanceAt( pixel );
	float sum = 0.0, total = 0.0;
	for( int y = -2; y <= 2; ++y )
	{
		for( int x = -2; x <= 2; ++x )
		{
			ivec2 at = clamp( pixel + ivec2( x, y ), ivec2( 0 ), size - 1 );
			float weight = exp( -abs( distanceAt( at ) - centre ) / max( centre * 0.02, 0.01 ) );
			sum += texelFetch( vkSampler2D( occlusion, occlusionSampler ), at, 0 ).x * weight;
			total += weight;
		}
	}
	smoothed = sum / total;
}
