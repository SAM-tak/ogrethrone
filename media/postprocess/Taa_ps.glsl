#version ogre_glsl_ver_330

// Temporal anti-aliasing: blends the jittered current frame into the history of earlier frames,
// reprojected by the motion vectors. It also writes each pixel's own motion since the previous
// frame, without the jitter and with the sky's from the camera, for the motion blur. The history is clipped to the colour range around the
// pixel (in YCoCg) so that it cannot show what is no longer there, and both are weighted by
// their inverse brightness so that bright pixels do not flicker.

vulkan_layout( ogre_t0 ) uniform texture2D current;
vulkan( layout( ogre_s0 ) uniform sampler currentSampler );
vulkan_layout( ogre_t1 ) uniform texture2D history;
vulkan( layout( ogre_s1 ) uniform sampler historySampler );
vulkan_layout( ogre_t2 ) uniform texture2D motion;
vulkan( layout( ogre_s2 ) uniform sampler motionSampler );
vulkan_layout( ogre_t3 ) uniform texture2D depth;
vulkan( layout( ogre_s3 ) uniform sampler depthSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The current frame's clip space (UV y as the targets store it) into the previous frame's.
	uniform mat4 reproject;
	// The projection's jitter (how far it moves the image), in UV, and its change since the
	// previous frame.
	uniform vec2 jitter;
	uniform vec2 jitterDelta;
	// The weight of the current frame: 1 shows it alone (no history yet, or TAA off).
	uniform float currentWeight;
	// 1 when the depth buffer is reversed (the far plane is 0).
	uniform float reverseDepth;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;
vulkan_layout( location = 1 )
out vec2 velocity;

vec3 toYCoCg( vec3 c )
{
	return vec3( dot( c, vec3( 0.25, 0.5, 0.25 ) ), dot( c, vec3( 0.5, 0.0, -0.5 ) ), dot( c, vec3( -0.25, 0.5, -0.25 ) ) );
}

vec3 fromYCoCg( vec3 c )
{
	return vec3( c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z );
}

bool isSky( float d )
{
	return d == ( reverseDepth > 0.5 ? 0.0 : 1.0 );
}

// The motion, in UV since the previous frame and without the jitter, of the surface at @p at
// (of depth @p d, centred at @p uv); where nothing was drawn (the sky), the camera's alone.
vec2 motionAt( ivec2 at, float d, vec2 uv )
{
	if( isSky( d ) )
	{
		vec4 previous = reproject * vec4( uv * 2.0 - 1.0, d, 1.0 );
		return uv - ( previous.xy / previous.w * 0.5 + 0.5 ) - jitterDelta;
	}
	return texelFetch( vkSampler2D( motion, motionSampler ), at, 0 ).xy - jitterDelta;
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( current, currentSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	vec2 uv = ( vec2( pixel ) + 0.5 ) / vec2( size );
	vec4 colour = texelFetch( vkSampler2D( current, currentSampler ), pixel, 0 );
	velocity = motionAt( pixel, texelFetch( vkSampler2D( depth, depthSampler ), pixel, 0 ).x, uv );
	if( currentWeight >= 1.0 )
	{
		fragColour = colour;
		return;
	}

	// The colour range around the pixel, the nearest surface's motion (so that edges move with
	// what is in front), and this frame's colour at the pixel's centre: each sample was taken
	// jitter away from its texel's centre, weighted by a Gaussian fit of Blackman-Harris.
	vec3 low = toYCoCg( colour.rgb ), high = low;
	vec3 filtered = vec3( 0.0 );
	float filteredWeight = 0.0;
	vec2 jitterPixels = jitter * vec2( size );
	ivec2 nearest = pixel;
	float nearestDepth = texelFetch( vkSampler2D( depth, depthSampler ), pixel, 0 ).x;
	for( int y = -1; y <= 1; ++y )
	{
		for( int x = -1; x <= 1; ++x )
		{
			ivec2 at = clamp( pixel + ivec2( x, y ), ivec2( 0 ), size - 1 );
			vec3 sampled = texelFetch( vkSampler2D( current, currentSampler ), at, 0 ).rgb;
			vec2 away = vec2( x, y ) - jitterPixels;
			float weight = exp( -2.29 * dot( away, away ) );
			filtered += sampled * weight;
			filteredWeight += weight;
			vec3 c = toYCoCg( sampled );
			low = min( low, c );
			high = max( high, c );
			float d = texelFetch( vkSampler2D( depth, depthSampler ), at, 0 ).x;
			if( reverseDepth > 0.5 ? d > nearestDepth : d < nearestDepth )
			{
				nearestDepth = d;
				nearest = at;
			}
		}
	}

	vec2 previousUv = uv - motionAt( nearest, nearestDepth, ( vec2( nearest ) + 0.5 ) / vec2( size ) );
	if( any( lessThan( previousUv, vec2( 0.0 ) ) ) || any( greaterThan( previousUv, vec2( 1.0 ) ) ) )
	{
		fragColour = colour;
		return;
	}

	// Clip the history towards the centre of the range, then blend.
	vec3 past = toYCoCg( texture( vkSampler2D( history, historySampler ), previousUv ).rgb );
	vec3 centre = ( low + high ) * 0.5, extent = max( ( high - low ) * 0.5, vec3( 1e-5 ) );
	vec3 offset = past - centre;
	vec3 scaled = abs( offset / extent );
	float outside = max( scaled.x, max( scaled.y, scaled.z ) );
	if( outside > 1.0 )
		past = centre + offset / outside;
	vec3 pastRgb = max( fromYCoCg( past ), vec3( 0.0 ) );

	colour.rgb = filtered / filteredWeight;
	float currentLuma = dot( colour.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
	float pastLuma = dot( pastRgb, vec3( 0.2126, 0.7152, 0.0722 ) );
	float wc = currentWeight / ( 1.0 + currentLuma );
	float wp = ( 1.0 - currentWeight ) / ( 1.0 + pastLuma );
	fragColour = vec4( ( colour.rgb * wc + pastRgb * wp ) / ( wc + wp ), colour.a );
}
