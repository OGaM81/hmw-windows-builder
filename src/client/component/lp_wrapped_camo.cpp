#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "console.hpp"
#include "filesystem.hpp"
#include "scheduler.hpp"

#include "game/game.hpp"
#include "game/dvars.hpp"

#include <utils/memory.hpp>
#include <utils/image.hpp>
#include <utils/string.hpp>

namespace lp_wrapped_camo
{
	namespace
	{
		constexpr const char* k_material_name = "camo_h1_lp_wrapped";
		constexpr int k_frame_count = 11;
		constexpr auto k_frame_interval = 100ms; // ~10 fps tick cadence

		game::dvar_t* r_lp_wrapped_camo_anim = nullptr;
		game::dvar_t* r_lp_wrapped_camo_fps = nullptr;

		game::GfxImage* frame_images[k_frame_count]{};
		bool frames_ready = false;
		bool frames_load_attempted = false;

		game::Material* target_material = nullptr;
		game::MaterialTextureDef* color_tex_def = nullptr;
		game::GfxImage* original_color_image = nullptr;

		int current_frame = 0;
		int last_applied_frame = -1;

		game::MaterialTextureDef* find_color_map(game::Material* material)
		{
			if (!material || !material->textureTable || material->textureCount == 0)
			{
				return nullptr;
			}

			game::MaterialTextureDef* col_by_semantic = nullptr;

			for (auto i = 0; i < material->textureCount; ++i)
			{
				auto* def = &material->textureTable[i];
				if (!def->u.image)
				{
					continue;
				}

				if (def->semantic != game::TS_COLOR_MAP)
				{
					continue;
				}

				const auto* name = def->u.image->name;
				if (name && (std::strstr(name, "_col") || std::strstr(name, "lp_wrapped")))
				{
					return def;
				}

				if (!col_by_semantic)
				{
					col_by_semantic = def;
				}
			}

			return col_by_semantic;
		}

		game::GfxImage* create_image_from_png(const std::string& asset_name, const std::string& png_data)
		{
			if (*game::d3d11_device == nullptr)
			{
				return nullptr;
			}

			utils::image raw_image{png_data};

			const auto image = utils::memory::allocate<game::GfxImage>();
			std::memset(image, 0, sizeof(*image));

			image->name = utils::memory::duplicate_string(asset_name);
			image->mapType = game::MAPTYPE_2D;
			image->semantic = static_cast<unsigned char>(game::TS_COLOR_MAP);
			image->category = static_cast<unsigned char>(game::IMG_CATEGORY_LOAD_FROM_FILE);
			image->flags = 0;
			image->depth = 1;
			image->numElements = 1;
			image->levelCount = 1;
			*(int*)&image->picmip = -1;

			D3D11_SUBRESOURCE_DATA data{};
			data.SysMemPitch = raw_image.get_width() * 4;
			data.SysMemSlicePitch = data.SysMemPitch * raw_image.get_height();
			data.pSysMem = raw_image.get_buffer();

			game::Image_Setup(image, raw_image.get_width(), raw_image.get_height(), image->depth,
				image->numElements, image->mapType, DXGI_FORMAT_R8G8B8A8_UNORM, image->name, &data);

			return image;
		}

		bool try_load_frames()
		{
			if (frames_ready)
			{
				return true;
			}

			if (*game::d3d11_device == nullptr)
			{
				return false;
			}

			int loaded = 0;

			for (auto i = 0; i < k_frame_count; ++i)
			{
				if (frame_images[i])
				{
					++loaded;
					continue;
				}

				const auto index = i + 1;
				const auto disk_name = utils::string::va("lp_wrapped_frame_%02d", index);
				const auto disk_path = utils::string::va("images/%s.png", disk_name);

				std::string png;
				if (filesystem::read_file(disk_path, &png) && !png.empty())
				{
					auto* image = create_image_from_png(disk_name, png);
					if (!image)
					{
						console::error("lp_wrapped_camo: Image_Setup failed for %s\n", disk_path);
						continue;
					}

					frame_images[i] = image;
					++loaded;
					continue;
				}

				// Optional fallback: zone-packed image assets with the same base name
				const auto header = game::DB_FindXAssetHeader(game::ASSET_TYPE_IMAGE, disk_name, false);
				if (header.image && !game::DB_IsXAssetDefault(game::ASSET_TYPE_IMAGE, disk_name))
				{
					frame_images[i] = header.image;
					++loaded;
					continue;
				}

				console::warn("lp_wrapped_camo: missing frame %s (place PNG at %s)\n", disk_name, disk_path);
			}

			frames_load_attempted = true;

			if (loaded < k_frame_count)
			{
				console::error("lp_wrapped_camo: only %d/%d frames available — animation waits for all frames\n",
					loaded, k_frame_count);
				return false;
			}

			frames_ready = true;
			console::info("lp_wrapped_camo: loaded %d flipbook frames for %s\n", k_frame_count, k_material_name);
			return true;
		}

		bool try_bind_material()
		{
			if (color_tex_def && target_material)
			{
				if (!game::DB_IsXAssetDefault(game::ASSET_TYPE_MATERIAL, k_material_name))
				{
					const auto header = game::DB_FindXAssetHeader(game::ASSET_TYPE_MATERIAL, k_material_name, false);
					if (header.material == target_material)
					{
						return true;
					}
				}

				if (color_tex_def && original_color_image)
				{
					color_tex_def->u.image = original_color_image;
				}
				target_material = nullptr;
				color_tex_def = nullptr;
				original_color_image = nullptr;
				last_applied_frame = -1;
			}

			const auto header = game::DB_FindXAssetHeader(game::ASSET_TYPE_MATERIAL, k_material_name, false);
			if (!header.material || game::DB_IsXAssetDefault(game::ASSET_TYPE_MATERIAL, k_material_name))
			{
				return false;
			}

			auto* color = find_color_map(header.material);
			if (!color || !color->u.image)
			{
				console::warn("lp_wrapped_camo: material %s has no COLOR_MAP texture\n", k_material_name);
				return false;
			}

			target_material = header.material;
			color_tex_def = color;
			original_color_image = color->u.image;
			last_applied_frame = -1;

			console::info("lp_wrapped_camo: bound COLOR_MAP '%s' on %s\n",
				original_color_image->name ? original_color_image->name : "?", k_material_name);
			return true;
		}

		void restore_original()
		{
			if (color_tex_def && original_color_image)
			{
				color_tex_def->u.image = original_color_image;
				last_applied_frame = -1;
			}
		}

		void apply_frame(const int frame)
		{
			if (!color_tex_def || !frames_ready)
			{
				return;
			}

			if (frame < 0 || frame >= k_frame_count || !frame_images[frame])
			{
				return;
			}

			color_tex_def->u.image = frame_images[frame];
			last_applied_frame = frame;
		}

		int resolved_fps()
		{
			auto fps = 10;
			if (r_lp_wrapped_camo_fps)
			{
				fps = r_lp_wrapped_camo_fps->current.integer;
			}
			if (fps < 1)
			{
				fps = 1;
			}
			if (fps > 60)
			{
				fps = 60;
			}
			return fps;
		}

		void tick()
		{
			if (!r_lp_wrapped_camo_anim || !r_lp_wrapped_camo_anim->current.enabled)
			{
				restore_original();
				return;
			}

			if (!frames_ready)
			{
				static auto last_try = std::chrono::steady_clock::time_point{};
				const auto now = std::chrono::steady_clock::now();
				if (!frames_load_attempted || (now - last_try) > 2s)
				{
					last_try = now;
					try_load_frames();
				}
				if (!frames_ready)
				{
					return;
				}
			}

			if (!try_bind_material())
			{
				return;
			}

			static auto last_advance = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();
			const auto interval = std::chrono::milliseconds(1000 / resolved_fps());

			if ((now - last_advance) >= interval)
			{
				last_advance = now;
				current_frame = (current_frame + 1) % k_frame_count;
			}

			if (current_frame != last_applied_frame)
			{
				apply_frame(current_frame);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			r_lp_wrapped_camo_anim = dvars::register_bool("r_lpWrappedCamoAnim", true,
				game::DVAR_FLAG_SAVED, "Animate camo_h1_lp_wrapped color-map flipbook");
			r_lp_wrapped_camo_fps = dvars::register_int("r_lpWrappedCamoFps", 10, 1, 60,
				game::DVAR_FLAG_SAVED, "Flipbook FPS for camo_h1_lp_wrapped (default 10)");

			scheduler::loop(tick, scheduler::pipeline::renderer, k_frame_interval);
		}
	};
}

REGISTER_COMPONENT(lp_wrapped_camo::component)
