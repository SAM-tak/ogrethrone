#version ogre_glsl_ver_330

// Screen-space subsurface scattering (TDDja/09), one direction of Jimenez's separable filter: the
// lit surface is blurred along the direction by a skin profile (d'Eon and Luebke's sum of six
// Gaussians, red spreading furthest) scaled to the surface's width. Each sample stands for a
// cell of the kernel, weighted by the profile's integral over it, and the cells move with the
// pixel's and the frame's noise for temporal anti-aliasing to average. A sample off the surface
// or at another depth takes the centre's colour, so light does not bleed across silhouettes.
// Pixels without subsurface scattering are copied by the horizontal pass and left alone by the
// vertical one, which draws over the scene.

vulkan_layout( ogre_t0 ) uniform texture2D colour;
vulkan( layout( ogre_s0 ) uniform sampler colourSampler );
vulkan_layout( ogre_t1 ) uniform texture2D subsurface;
vulkan( layout( ogre_s1 ) uniform sampler subsurfaceSampler );
vulkan_layout( ogre_t2 ) uniform texture2D depth;
vulkan( layout( ogre_s2 ) uniform sampler depthSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// (1, 0) or (0, 1).
	uniform vec2 direction;
	// Pixels per metre at a distance of one metre, horizontally and vertically.
	uniform vec2 pixelScale;
	// The projection's z row (A, B): the distance to a depth d is B / (d + A).
	uniform vec2 depthParams;
	uniform float frameNoise;
	// 1 when the pass draws over the scene: pixels without scattering are discarded.
	uniform float vertical;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 result;

const int samples = 11;
// The profile's extent in millimetres (three deviations of its widest Gaussian), which a
// surface's width stretches to.
const float extent = 8.0;

float distanceAt( ivec2 at )
{
	return depthParams.y / ( texelFetch( vkSampler2D( depth, depthSampler ), at, 0 ).x + depthParams.x );
}

// The standard normal distribution function, with Abramowitz and Stegun's erf (7.1.26).
float normalCdf( float x )
{
	float z = abs( x ) * 0.70710678;
	float t = 1.0 / ( 1.0 + 0.3275911 * z );
	float erf = 1.0 - ( ( ( ( 1.061405429 * t - 1.453152027 ) * t + 1.421413741 ) * t - 0.284496736 ) * t +
						0.254829592 ) * t * exp( -z * z );
	return 0.5 + 0.5 * sign( x ) * erf;
}

// The skin profile's integral from a to b millimetres along one axis, per channel.
vec3 profile( float a, float b )
{
	const float variances[6] = float[6]( 0.0064, 0.0484, 0.187, 0.567, 1.99, 7.41 );
	const vec3 weights[6] = vec3[6]( vec3( 0.233, 0.455, 0.649 ), vec3( 0.100, 0.336, 0.344 ),
									 vec3( 0.118, 0.198, 0.0 ), vec3( 0.113, 0.007, 0.007 ),
									 vec3( 0.358, 0.004, 0.0 ), vec3( 0.078, 0.0, 0.0 ) );
	vec3 sum = vec3( 0.0 );
	for( int i = 0; i < 6; ++i )
	{
		float deviation = sqrt( variances[i] );
		sum += weights[i] * ( normalCdf( b / deviation ) - normalCdf( a / deviation ) );
	}
	return sum;
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( colour, colourSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	vec4 centre = texelFetch( vkSampler2D( colour, colourSampler ), pixel, 0 );
	float width = texelFetch( vkSampler2D( subsurface, subsurfaceSampler ), pixel, 0 ).x;
	float centreDistance = distanceAt( pixel );
	// The kernel's radius in pixels.
	float radius = min( width * dot( pixelScale, direction ) / centreDistance, 128.0 );
	if( width <= 0.0 || radius < 0.5 )
	{
		if( vertical > 0.5 )
			discard;
		result = centre;
		return;
	}
	float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) + frameNoise );
	float cell = 2.0 / float( samples );
	vec3 sum = vec3( 0.0 ), total = vec3( 0.0 );
	for( int i = 0; i < samples; ++i )
	{
		// The cell in [-1, 1] (shifted by up to half a cell) and its sample at the middle.
		float low = -1.0 + ( float( i ) + noise - 0.5 ) * cell;
		vec3 weight = profile( low * extent, ( low + cell ) * extent );
		ivec2 at = clamp( pixel + ivec2( round( direction * ( low + cell * 0.5 ) * radius ) ), ivec2( 0 ), size - 1 );
		vec3 taken = texelFetch( vkSampler2D( colour, colourSampler ), at, 0 ).xyz;
		float onSurface = texelFetch( vkSampler2D( subsurface, subsurfaceSampler ), at, 0 ).x > 0.0 ? 1.0 : 0.0;
		float follow = onSurface * ( 1.0 - clamp( abs( distanceAt( at ) - centreDistance ) / width, 0.0, 1.0 ) );
		sum += weight * mix( centre.xyz, taken, follow );
		total += weight;
	}
	result = vec4( sum / max( total, vec3( 1e-6 ) ), centre.w );
}
