#version ogre_glsl_ver_330

// Scales the half-float scene by the exposure and, when filmic is set, maps it
// through the ACES filmic curve; otherwise it is clamped as an LDR target would.

vulkan_layout( ogre_t0 ) uniform texture2D scene;
vulkan( layout( ogre_s0 ) uniform sampler sceneSampler );
vulkan_layout( ogre_t1 ) uniform texture2D bloom;
vulkan( layout( ogre_s1 ) uniform sampler bloomSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform float exposure;
	uniform float filmic;
	uniform float bloomStrength;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

// Krzysztof Narkowicz's fit of the ACES filmic tone curve.
vec3 aces( vec3 x )
{
	return clamp( ( x * ( 2.51 * x + 0.03 ) ) / ( x * ( 2.43 * x + 0.59 ) + 0.14 ), 0.0, 1.0 );
}

void main()
{
	vec4 colour = texture( vkSampler2D( scene, sceneSampler ), inPs.uv0 );
	vec3 glow = texture( vkSampler2D( bloom, bloomSampler ), inPs.uv0 ).rgb;
	vec3 exposed = (colour.rgb + glow * bloomStrength) * exposure;
	fragColour = vec4( filmic > 0.5 ? aces( exposed ) : clamp( exposed, 0.0, 1.0 ), colour.a );
}
