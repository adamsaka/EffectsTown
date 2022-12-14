/********************************************************************************************************

Authors:		(c) 2022 Maths Town

Licence:		The MIT License

*********************************************************************************************************
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
********************************************************************************************************

Description:

    Render functions for After Effects host.

*******************************************************************************************************/

//Project Specific includes
#include "config.h"
#include "renderer.h"
#include "parameters.h"

//General Includes
#include "after-effects-render.h"
#include "after-effects-parameter-helper.h"
#include "..\..\common\util.h"

#include "..\..\common\simd-cpuid.h"
#include "..\..\common\simd-f32.h"
#include "..\..\common\simd-uint32.h"


template <SimdFloat S>
struct RenderData {
	int width{};
	int height{};
	Renderer<S> renderer;
	PF_Rect area;
	PF_EffectWorld* inputLayer{};
	PF_EffectWorld* output{};
	A_u_long rowbytes{};


};


/*******************************************************************************************************
Converts a colour component from the renderer to an 8-bit component
*******************************************************************************************************/
inline static uint8_t to_adobe_8bit(Precision c) { 
	constexpr unsigned short adobe_white8 = 0xff;
	return static_cast<uint8_t>(clamp(c * white8, static_cast<Precision>(0.0), static_cast<Precision>(adobe_white8)));
}

/*******************************************************************************************************
Converts a colour component from the renderer to an 16-bit component
Note: Adobe 16 bit is not full 16-bit.  White is 0x8000
*******************************************************************************************************/
inline static uint16_t to_adobe_16bit(Precision c) {
	constexpr unsigned short adobe_white16 = 0x8000;
	return static_cast<uint16_t>(clamp(c * adobe_white16, static_cast<Precision>(0.0), static_cast<Precision>(adobe_white16)));
}





/*******************************************************************************************************
Callback for After Effects Iteration Suite.  Renders an 8-bit pixel.
*******************************************************************************************************/
template <SimdFloat S>
static PF_Err render_8bit_pixel_callback(void* refcon, A_long thread_idxL, A_long  i,	A_long itrtL) noexcept {
	

	



	/*const auto renderer = static_cast<Renderer<Precision>*>(refcon);
	const auto c = renderer->render_pixel(x, y);
	
	//Note: Adobe uses ARGB colour order, with unmultiplied alpha.
	out->alpha = to_adobe_8bit(c.alpha);
	out->red = to_adobe_8bit(c.red);
	out->green = to_adobe_8bit(c.green);
	out->blue = to_adobe_8bit(c.blue);*/
	return PF_Err_NONE;

}

/*******************************************************************************************************
Callback for After Effects Iteration Suite.  Renders an 16-bit pixel.
*******************************************************************************************************/
template <SimdFloat S>
static PF_Err render_16bit_pixel_callback(void* refcon, A_long thread_idxL, A_long  i, A_long itrtL) noexcept {
	/*const auto renderer = static_cast<Renderer<Precision>*>(refcon);
	const auto c = renderer->render_pixel(x, y);

	//Note: Adobe uses ARGB colour order, with unmultiplied alpha.
	out->alpha = to_adobe_16bit(c.alpha);
	out->red = to_adobe_16bit(c.red);
	out->green = to_adobe_16bit(c.green);
	out->blue = to_adobe_16bit(c.blue);*/
	return PF_Err_NONE;
}

template <SimdFloat S>
void copy_to_output_32(PF_EffectWorld* output,int x, int y, int max_x, const ColourSRGB<S>& c) {
	auto ptrY = (uint8_t*)output->data;
	ptrY += y * output->rowbytes;
	
	for (int i = 0; i < S::number_of_elements(); i++) {
		if (x + i >= max_x) break;
		auto ptrByte = ptrY + (x + i) * 4 * sizeof(float);

		auto ptrFloat = (float*)ptrByte;
		
		ptrFloat[0] = static_cast<float>(c.alpha.element(i));
		ptrFloat[1] = static_cast<float>(c.red.element(i));
		ptrFloat[2] = static_cast<float>(c.green.element(i));
		ptrFloat[3] = static_cast<float>(c.blue.element(i));
		
		
	}
}

/*******************************************************************************************************
Callback for After Effects Iteration Suite.  Renders an 32-bit pixel.
*******************************************************************************************************/
template <SimdFloat S>
static PF_Err render_32bit_pixel_callback(void* refcon, A_long thread_idxL, A_long  i, A_long itrtL) noexcept {
	const auto rd = static_cast<RenderData<S> *>(refcon);
	const auto y = i;

	if (y < rd->area.top || y>= rd->area.bottom) return PF_Err_NONE;
	
	for (int x = rd->area.left  ; x < rd->area.right; x += S::number_of_elements() ) {
		const auto c = rd->renderer.render_pixel(S::make_sequential(static_cast<S::F>(x)), S::make_set1(static_cast<S::F>(y)));
		copy_to_output_32(rd->output, x, y, rd->area.right, c);
	}

	

	/*const ColourSRGB c{}; //renderer->render_pixel(x, y);

	//Note: Adobe uses ARGB colour order, with unmultiplied alpha.
	out->alpha = static_cast<float>(c.alpha);
	out->red = static_cast<float>(c.red);
	out->green = static_cast<float>(c.green);
	out->blue = static_cast<float>(c.blue);
	*/
	
	return PF_Err_NONE;

}


/*******************************************************************************************************
Calculates the width and height of the image we are working with.  (From layer size)
Size may vary in low resolution renders.
*******************************************************************************************************/
static std::tuple<int, int> calculate_size(const PF_InData* in_data) {
	//We may be redering a low resolution version, get the scaleFactor
	const float scaleFactorX = static_cast<float>(in_data->downsample_x.den) / static_cast<float>(in_data->downsample_x.num);
	const float scaleFactorY = static_cast<float>(in_data->downsample_y.den) / static_cast<float>(in_data->downsample_y.num);

	//Calculate the size of our output using the full layer size.
	int width = static_cast<int>(static_cast<float>(in_data->width) / scaleFactorX);
	int height = static_cast<int>(static_cast<float>(in_data->height) / scaleFactorY);

	return std::tuple<int, int>(width, height);
}

/*******************************************************************************************************
Read Parameters from After Effects and put into data into our parameter list.
*******************************************************************************************************/
ParameterList read_parameters() {
	auto params = build_project_parameters();
	for (auto & p : params.entries) {
		switch (p.type) {
		case ParameterType::seed:
			p.value = ParameterHelper::ReadSlider(p.id);
			break;
		case ParameterType::number:
			p.value = ParameterHelper::ReadSlider(p.id);			
			break;

		default:
			break;
		}
	}

	return params;
}


/*******************************************************************************************************
Setup Host Independant Renderer
*******************************************************************************************************/
template <typename S>
void setup_render(Renderer<S>& renderer, const PF_InData* in_data, int width, int height) {
	check_null(in_data);
	auto params = read_parameters();
	
	renderer.set_size(width, height);
	renderer.set_seed("After Effects");
	if (params.contains(ParameterID::seed)) {
		renderer.set_seed_int(static_cast<uint32_t>(params.get_value(ParameterID::seed)));
	}
	
	renderer.set_parameters(std::move(params));

}

/*******************************************************************************************************
After Effects SmartPreRender Command
Called by After Effects to get information about the render.  
We need to set the output rectangles and checkout any layers that we need.
*******************************************************************************************************/
void after_effects_smart_pre_render(const PF_InData* in_data, PF_PreRenderExtra* preRender) {
	check_null(in_data);
	check_null(preRender);
	check_null(in_data->effect_ref);
	check_null(preRender->output);
	check_null(preRender->input);

	const auto [width, height] = calculate_size(in_data);

	//Checkout the input layer
	PF_CheckoutResult inputLayer{};
	if constexpr (project_uses_input) {
		check_after_effects(preRender->cb->checkout_layer(in_data->effect_ref, 0, 0, &preRender->input->output_request, in_data->current_time, in_data->time_step, in_data->time_scale, &inputLayer));
	}
	const auto r = preRender->input->output_request.rect;
	//const auto in = inputLayer.result_rect;
	


	//Max output rectangle.
	preRender->output->max_result_rect.top = 0;
	preRender->output->max_result_rect.bottom = height;
	preRender->output->max_result_rect.left = 0;
	preRender->output->max_result_rect.right = width;
	
	//Output rectangle
	preRender->output->result_rect.top = std::max(0, r.top);
	preRender->output->result_rect.bottom = std::min(height, r.bottom);
	preRender->output->result_rect.left = std::max(0, r.left);
	preRender->output->result_rect.right = std::min(width, r.right);

	//Is output solid?
	preRender->output->solid = project_is_solid_render; //Project setting from config.h
}


/*******************************************************************************************************
Template function built based on SIMD type (which is CPU dependant)
*******************************************************************************************************/
template <SimdFloat S>
void after_effect_cpu_dispatch(int width, int height, PF_InData* in_data, const PF_Rect& area, int bit_depth, PF_EffectWorld* inputLayer, PF_EffectWorld* output, RenderData<S>& rd) {
	setup_render(rd.renderer, in_data, width, height);
	
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	switch (bit_depth) {
	case 8:
	{
		check_after_effects(suites.Iterate8Suite1()->iterate_generic(rd.height, &rd , render_8bit_pixel_callback<S>));
		break;
	}
	case 16: {
		check_after_effects(suites.Iterate8Suite1()->iterate_generic(rd.height, &rd, render_16bit_pixel_callback<S>));
		break;
	}
	case 32: {
		check_after_effects(suites.Iterate8Suite1()->iterate_generic(rd.height, &rd, render_32bit_pixel_callback<S>));
		break;
	}
	default: break;
	}
}


/*******************************************************************************************************
Common Render Function to Smart and Non-Smart rendering.
Sets up the renderer and dispatches based on CPU
*******************************************************************************************************/
void after_effects_common_render(int width, int height, PF_InData* in_data, const PF_Rect& area, int bit_depth, PF_EffectWorld* inputLayer, PF_EffectWorld* output) {
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	//suites.WorldSuite3()->AEGP_GetBaseAddr32();
	

	CpuInformation cpu_info{};
	if (Simd512UInt32::cpu_supported(cpu_info) && Simd512Float32::cpu_supported(cpu_info)) {
		//AVX-512 & AVX-512DQ 
		RenderData<Simd512Float32> rd{};
		rd.width = width;
		rd.height = height;
		rd.area = area;
		rd.output = output;
		rd.inputLayer = inputLayer;
		after_effect_cpu_dispatch(width, height, in_data, area, bit_depth,  inputLayer, output, rd);

	}
	else if (Simd256UInt32::cpu_supported(cpu_info) && Simd256Float32::cpu_supported(cpu_info)) {
		//AVX & AVX2
		RenderData<Simd256Float32> rd{};
		rd.width = width;
		rd.height = height;
		rd.area = area;
		rd.output = output;
		rd.inputLayer = inputLayer;
		after_effect_cpu_dispatch(width, height, in_data, area, bit_depth, inputLayer, output, rd);
	}
	else {
		//Fallback
		RenderData<FallbackFloat32> rd{};
		rd.width = width;
		rd.height = height;
		rd.area = area;
		rd.output = output;
		rd.inputLayer = inputLayer;
		after_effect_cpu_dispatch(width, height, in_data, area, bit_depth, inputLayer, output, rd);
	}
}


/*******************************************************************************************************
After Effects Smart Render Command
This render likely comes from After Effects.
*******************************************************************************************************/
void after_effects_smart_render(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* smartRender) {
	check_null(in_data);
	check_null(out_data);
	check_null(smartRender);

	const auto [width, height] = calculate_size(in_data);

	//Checkout the input buffer
	PF_EffectWorld* inputLayer{ nullptr };
	if constexpr (project_uses_input) {
		check_after_effects(smartRender->cb->checkout_layer_pixels(in_data->effect_ref, 0, &inputLayer));
		if (!inputLayer) throw (std::exception("Unable to checkout input layer."));
	}

	//Checkout Output buffer
	PF_EffectWorld* output{ nullptr };
	check_after_effects(smartRender->cb->checkout_output(in_data->effect_ref, &output));
	if (!output) throw (std::exception("Unable to checkout input layer."));

	//Area of interest
	PF_Rect area;
	area.left = 0;
	area.right = output->width;
	area.top = 0;
	area.bottom = output->height;
	
	

	after_effects_common_render(width, height, in_data, area, smartRender->input->bitdepth, inputLayer, output);	
}

/*******************************************************************************************************
After Effects Render Command (Old fashioned Non-Smart Mode)
This render likely comes from Premiere Pro.
*******************************************************************************************************/
void after_effects_non_smart_render(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output) {
	check_null(in_data);
	check_null(out_data);
	check_null(output);
	check_null(params);

	const auto [width, height] = calculate_size(in_data);
	
	//Area of interest
	PF_Rect area;
	area.left = 0;
	area.right = width;
	area.top = 0;
	area.bottom = height;

	auto inputLayer = &params[0]->u.ld;
	
	int bit_depth = (output->world_flags & PF_WorldFlag_DEEP) ? 16 : 8;

	after_effects_common_render(width, height, in_data, area, bit_depth, inputLayer, output);
}