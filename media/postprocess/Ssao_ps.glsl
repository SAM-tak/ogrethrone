#version ogre_glsl_ver_330

// Screen-space ambient occlusion from the depth prepass: the share of a hemisphere around each
// surface's normal, within a radius, that other surfaces in the depth buffer do not cover. The
// samples turn per pixel by interleaved gradient noise; SsaoBlur smooths the grain away.

vulkan_layout( ogre_t0 ) uniform texture2D normals;
vulkan( layout( ogre_s0 ) uniform sampler normalSampler );
vulkan_layout( ogre_t1 ) uniform texture2D depth;
vulkan( layout( ogre_s1 ) uniform sampler depthSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// The view's projection (into clip space with UV y as the targets store it) and its inverse.
	uniform mat4 projection;
	uniform mat4 inverseProjection;
	// The hemisphere's radius in metres, and how strongly occlusion darkens (0: none).
	uniform float radius;
	uniform float strength;
	// 1 when the depth buffer is reversed (the far plane is 0).
	uniform float reverseDepth;
	// Changes every frame, so temporal anti-aliasing averages the noise away.
	uniform float frameNoise;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float occlusion;

const int sampleCount = 12;

vec3 viewPosition( vec2 uv, float d )
{
	vec4 position = inverseProjection * vec4( uv * 2.0 - 1.0, d, 1.0 );
	return position.xyz / position.w;
}

void main()
{
	ivec2 size = textureSize( vkSampler2D( depth, depthSampler ), 0 );
	ivec2 pixel = clamp( ivec2( gl_FragCoord.xy ), ivec2( 0 ), size - 1 );
	float d = texelFetch( vkSampler2D( depth, depthSampler ), pixel, 0 ).x;
	if( strength <= 0.0 || d == ( reverseDepth > 0.5 ? 0.0 : 1.0 ) )
	{
		occlusion = 1.0;
		return;
	}
	vec2 uv = ( vec2( pixel ) + 0.5 ) / vec2( size );
	vec3 position = viewPosition( uv, d );
	vec3 normal = normalize( texelFetch( vkSampler2D( normals, normalSampler ), pixel, 0 ).xyz * 2.0 - 1.0 );

	// A tangent frame turned by the noise.
	float angle = 6.2831853 * fract( fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) ) + frameNoise );
	vec3 turned = vec3( cos( angle ), sin( angle ), 0.0 );
	vec3 tangent = normalize( turned - normal * dot( turned, normal ) + vec3( 1e-4, 0.0, 0.0 ) );
	vec3 bitangent = cross( normal, tangent );

	float covered = 0.0;
	for( int i = 0; i < sampleCount; ++i )
	{
		// Points of a Fibonacci spiral over the hemisphere, nearer the centre more often.
		float t = ( float( i ) + 0.5 ) / float( sampleCount );
		float cosTheta = 1.0 - t;
		float sinTheta = sqrt( 1.0 - cosTheta * cosTheta );
		float phi = 2.3999632 * float( i );
		vec3 direction = vec3( cos( phi ) * sinTheta, sin( phi ) * sinTheta, cosTheta );
		float scale = mix( 0.1, 1.0, t * t );
		vec3 point = position + ( tangent * direction.x + bitangent * direction.y + normal * direction.z ) * radius * scale;

		vec4 clip = projection * vec4( point, 1.0 );
		vec2 at = clip.xy / clip.w * 0.5 + 0.5;
		if( any( lessThan( at, vec2( 0.0 ) ) ) || any( greaterThan( at, vec2( 1.0 ) ) ) )
			continue;
		ivec2 texel = clamp( ivec2( at * vec2( size ) ), ivec2( 0 ), size - 1 );
		vec3 surface = viewPosition( at, texelFetch( vkSampler2D( depth, depthSampler ), texel, 0 ).x );
		// The view looks along -z: a surface nearer the camera than the point covers it, unless
		// it lies much farther in front than the radius.
		float inRange = smoothstep( 0.0, 1.0, radius / max( abs( position.z - surface.z ), 1e-4 ) );
		covered += surface.z >= point.z + 0.02 * radius ? inRange : 0.0;
	}
	occlusion = clamp( 1.0 - strength * covered / float( sampleCount ), 0.0, 1.0 );
}
