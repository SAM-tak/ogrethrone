#version ogre_glsl_ver_330

// Depth of field of a thin lens: each pixel gathers the samples, over a disc, whose circle of
// confusion reaches it (scatter as gather), each weighted by the inverse of its circle's area so
// that a blurred point spreads its light rather than adding it. A sample behind the pixel
// spreads no farther than the pixel's own circle, so the background does not bleed over a
// sharp foreground, while a blurred foreground still spreads over the background.

vulkan_layout( ogre_t0 ) uniform texture2D scene;
vulkan( layout( ogre_s0 ) uniform sampler sceneSampler );
vulkan_layout( ogre_t1 ) uniform texture2D depth;
vulkan( layout( ogre_s1 ) uniform sampler depthSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The distance in focus, in metres.
	uniform float focusDistance;
	// The circle of confusion's diameter in pixels is cocScale * (z - focusDistance) / z; 0 is off.
	uniform float cocScale;
	// The largest circle's radius, in pixels.
	uniform float maxRadius;
	// The projection's z row (A, B): the distance to a depth d is B / (d + A).
	uniform vec2 depthParams;
	// Changes every frame, so temporal anti-aliasing averages the noise away.
	uniform float frameNoise;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

const int sampleCount = 32;

/// The signed radius, in pixels, of the circle of confusion at @p at: negative in front of the focus.
float radiusAt( ivec2 at )
{
	float z = depthParams.y / ( texelFetch( vkSampler2D( depth, depthSampler ), at, 0 ).x + depthParams.x );
	return clamp( 0.5 * cocScale * ( z - focusDistance ) / z, -maxRadius, maxRadius );
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( scene, sceneSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	vec4 colour = texelFetch( vkSampler2D( scene, sceneSampler ), pixel, 0 );
	if( cocScale <= 0.0 )
	{
		fragColour = colour;
		return;
	}
	float centre = radiusAt( pixel );
	float centreReach = max( abs( centre ), 0.5 );
	float total = 1.0 / ( centreReach * centreReach );
	vec3 sum = colour.rgb * total;
	// Points of a golden-angle spiral over the largest circle, turned per pixel by noise.
	float turn = 6.2831853 * fract( fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) ) + frameNoise );
	for( int i = 0; i < sampleCount; ++i )
	{
		float r = sqrt( ( float( i ) + 0.5 ) / float( sampleCount ) ) * maxRadius;
		float a = 2.3999632 * float( i ) + turn;
		vec2 offset = vec2( cos( a ), sin( a ) ) * r;
		ivec2 at = clamp( pixel + ivec2( round( offset ) ), ivec2( 0 ), size - 1 );
		float radius = radiusAt( at );
		float reach = abs( radius );
		// Behind the pixel: no farther than the pixel's own circle.
		if( radius > centre )
			reach = min( reach, centreReach );
		reach = max( reach, 0.5 );
		float covers = clamp( reach - length( offset ) + 1.0, 0.0, 1.0 );
		float weight = covers / ( reach * reach );
		sum += texelFetch( vkSampler2D( scene, sceneSampler ), at, 0 ).rgb * weight;
		total += weight;
	}
	fragColour = vec4( sum / total, colour.a );
}
