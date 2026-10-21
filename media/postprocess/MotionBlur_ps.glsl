#version ogre_glsl_ver_330

// Motion blur along the motion around each pixel since the previous frame (McGuire et al. 2012,
// "A Reconstruction Filter for Plausible Motion Blur"): samples along the fastest motion of the
// neighbouring tiles (MotionTiles) count where they are in front and reach the pixel with their
// own motion, or are behind and covered by its motion, so a moving object blurs over the
// background around it but a static foreground stays sharp over a moving background.

vulkan_layout( ogre_t0 ) uniform texture2D scene;
vulkan( layout( ogre_s0 ) uniform sampler sceneSampler );
vulkan_layout( ogre_t1 ) uniform texture2D velocities;
vulkan( layout( ogre_s1 ) uniform sampler velocitySampler );
vulkan_layout( ogre_t2 ) uniform texture2D depth;
vulkan( layout( ogre_s2 ) uniform sampler depthSampler );
vulkan_layout( ogre_t3 ) uniform texture2D neighbours;
vulkan( layout( ogre_s3 ) uniform sampler neighbourSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The share of the frame the shutter is open (0.5: a 180 degree shutter); 0 turns it off.
	uniform float shutter;
	// The projection's z row (A, B): the distance to a depth d is B / (d + A).
	uniform vec2 depthParams;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

const int sampleCount = 12;
/// The longest blur, in pixels.
const float maxBlur = 32.0;

float distanceAt( ivec2 at )
{
	return depthParams.y / ( texelFetch( vkSampler2D( depth, depthSampler ), at, 0 ).x + depthParams.x );
}

/// How much a (at distance za) is in front of b, softly.
float closer( float za, float zb )
{
	return clamp( 1.0 - ( za - zb ) / max( zb * 0.01, 0.01 ), 0.0, 1.0 );
}

float cone( float away, float reach )
{
	return clamp( 1.0 - away / reach, 0.0, 1.0 );
}

float cylinder( float away, float reach )
{
	return 1.0 - smoothstep( 0.95 * reach, 1.05 * reach, away );
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( scene, sceneSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	vec4 colour = texelFetch( vkSampler2D( scene, sceneSampler ), pixel, 0 );
	// The blur in pixels, the motion over the open shutter: the fastest around (along which the
	// samples lie) and the pixel's own.
	// Interleaved gradient noise offsets the samples per pixel, turning banding into grain, and
	// the tile looked up, hiding the tiles' edges.
	float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) ) - 0.5;
	ivec2 tiles = textureSize( vkSampler2D( neighbours, neighbourSampler ), 0 );
	ivec2 tile = clamp( ( pixel + ivec2( noise * 8.0, -noise * 8.0 ) ) / 16, ivec2( 0 ), tiles - 1 );
	vec2 blur = texelFetch( vkSampler2D( neighbours, neighbourSampler ), tile, 0 ).xy * vec2( size ) * shutter;
	float longest = length( blur );
	if( longest > maxBlur )
		blur *= maxBlur / longest;
	if( longest < 0.5 )
	{
		fragColour = colour;
		return;
	}
	float reach = clamp( length( texelFetch( vkSampler2D( velocities, velocitySampler ), pixel, 0 ).xy * vec2( size ) * shutter ),
						 0.5, maxBlur );

	float z = distanceAt( pixel );
	float total = 1.0 / reach;
	vec3 sum = colour.rgb * total;
	for( int i = 0; i < sampleCount; ++i )
	{
		float t = ( float( i ) + 0.5 + noise ) / float( sampleCount ) - 0.5;
		vec2 offset = blur * t;
		ivec2 at = clamp( pixel + ivec2( round( offset ) ), ivec2( 0 ), size - 1 );
		if( at == pixel )
			continue;
		float away = length( offset );
		float zs = distanceAt( at );
		vec2 sampleBlur = texelFetch( vkSampler2D( velocities, velocitySampler ), at, 0 ).xy * vec2( size ) * shutter;
		float sampleReach = clamp( length( sampleBlur ), 0.5, maxBlur );
		float weight = closer( zs, z ) * cone( away, sampleReach ) + closer( z, zs ) * cone( away, reach ) +
					   cylinder( away, sampleReach ) * cylinder( away, reach ) * 2.0;
		sum += texelFetch( vkSampler2D( scene, sceneSampler ), at, 0 ).rgb * weight;
		total += weight;
	}
	fragColour = vec4( sum / total, colour.a );
}
