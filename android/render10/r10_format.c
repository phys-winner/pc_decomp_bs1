/* DXGI formats of the Direct3D 10 path with their Vulkan and OpenGL ES counterparts, and a BC1-5
 * decoder for drivers without BC textures (the system Vulkan and OpenGL ES drivers of Adreno). */
#include "r10.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>

#ifndef GL_RGBA16_EXT
#define GL_RGBA16_EXT 0x805B
#define GL_RG16_EXT 0x822C
#define GL_R16_EXT 0x822A
#define GL_RGBA16_SNORM_EXT 0x8F9B
#define GL_RG16_SNORM_EXT 0x8F99
#define GL_R16_SNORM_EXT 0x8F98
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#ifndef GL_COMPRESSED_RED_RGTC1_EXT
#define GL_COMPRESSED_RED_RGTC1_EXT 0x8DBB
#define GL_COMPRESSED_SIGNED_RED_RGTC1_EXT 0x8DBC
#define GL_COMPRESSED_RED_GREEN_RGTC2_EXT 0x8DBD
#define GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT 0x8DBE
#endif

#define C R10_FMT_COLOR
#define D R10_FMT_DEPTH
#define B R10_FMT_BC
#define T R10_FMT_TYPELESS
/* dxgi, kind, bytes, block, bc, srgb, stencil, int, bgra, vk, family, gl internal, gl format, gl type, swizzle, alpha one, r->a */
#define F(dxgi, kind, bytes, block, bc, srgb, st, in, bgra, vk, fam, gli, glf, glt, sw, a1, ra, name) \
    { dxgi, kind, bytes, block, bc, srgb, st, in, bgra, vk, fam, gli, glf, glt, sw, a1, ra, 0, name }
static const r10_format formats[] = {
    F(1, T, 16, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 1, GL_RGBA32F, GL_RGBA, GL_FLOAT, 0, 0, 0, "R32G32B32A32_TYPELESS"),
    F(2, C, 16, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 1, GL_RGBA32F, GL_RGBA, GL_FLOAT, 0, 0, 0, "R32G32B32A32_FLOAT"),
    F(3, C, 16, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32B32A32_UINT, 1, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, "R32G32B32A32_UINT"),
    F(4, C, 16, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32B32A32_SINT, 1, GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 0, 0, 0, "R32G32B32A32_SINT"),
    F(5, T, 12, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 5, GL_RGB32F, GL_RGB, GL_FLOAT, 0, 0, 0, "R32G32B32_TYPELESS"),
    F(6, C, 12, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 5, GL_RGB32F, GL_RGB, GL_FLOAT, 0, 0, 0, "R32G32B32_FLOAT"),
    F(7, C, 12, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32B32_UINT, 5, GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, "R32G32B32_UINT"),
    F(8, C, 12, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32B32_SINT, 5, GL_RGB32I, GL_RGB_INTEGER, GL_INT, 0, 0, 0, "R32G32B32_SINT"),
    F(9, T, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 9, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0, 0, 0, "R16G16B16A16_TYPELESS"),
    F(10, C, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16B16A16_SFLOAT, 9, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 0, 0, 0, "R16G16B16A16_FLOAT"),
    F(11, C, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16B16A16_UNORM, 9, GL_RGBA16_EXT, GL_RGBA, GL_UNSIGNED_SHORT, 0, 0, 0, "R16G16B16A16_UNORM"),
    F(12, C, 8, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16G16B16A16_UINT, 9, GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, "R16G16B16A16_UINT"),
    F(13, C, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16B16A16_SNORM, 9, GL_RGBA16_SNORM_EXT, GL_RGBA, GL_SHORT, 0, 0, 0, "R16G16B16A16_SNORM"),
    F(14, C, 8, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16G16B16A16_SINT, 9, GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, 0, 0, 0, "R16G16B16A16_SINT"),
    F(15, T, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32_SFLOAT, 15, GL_RG32F, GL_RG, GL_FLOAT, 0, 0, 0, "R32G32_TYPELESS"),
    F(16, C, 8, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32G32_SFLOAT, 15, GL_RG32F, GL_RG, GL_FLOAT, 0, 0, 0, "R32G32_FLOAT"),
    F(17, C, 8, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32_UINT, 15, GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, "R32G32_UINT"),
    F(18, C, 8, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32G32_SINT, 15, GL_RG32I, GL_RG_INTEGER, GL_INT, 0, 0, 0, "R32G32_SINT"),
    F(19, T, 8, 1, 0, 0, 1, 0, 0, VK_FORMAT_D32_SFLOAT_S8_UINT, 19, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, 0, 0, "R32G8X24_TYPELESS"),
    F(20, D, 8, 1, 0, 0, 1, 0, 0, VK_FORMAT_D32_SFLOAT_S8_UINT, 19, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, 0, 0, "D32_FLOAT_S8X24_UINT"),
    F(21, D, 8, 1, 0, 0, 1, 0, 0, VK_FORMAT_D32_SFLOAT_S8_UINT, 19, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, 0, 0, "R32_FLOAT_X8X24_TYPELESS"),
    F(23, T, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 23, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 0, 0, 0, "R10G10B10A2_TYPELESS"),
    F(24, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 23, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 0, 0, 0, "R10G10B10A2_UNORM"),
    F(25, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_A2B10G10R10_UINT_PACK32, 23, GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, 0, 0, 0, "R10G10B10A2_UINT"),
    F(26, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 26, GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 0, 0, 0, "R11G11B10_FLOAT"),
    F(27, T, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, 27, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8B8A8_TYPELESS"),
    F(28, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8B8A8_UNORM, 27, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8B8A8_UNORM"),
    F(29, C, 4, 1, 0, 1, 0, 0, 0, VK_FORMAT_R8G8B8A8_SRGB, 27, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8B8A8_UNORM_SRGB"),
    F(30, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8G8B8A8_UINT, 27, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8B8A8_UINT"),
    F(31, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8B8A8_SNORM, 27, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, 0, 0, 0, "R8G8B8A8_SNORM"),
    F(32, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8G8B8A8_SINT, 27, GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, 0, 0, 0, "R8G8B8A8_SINT"),
    F(33, T, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16_SFLOAT, 33, GL_RG16F, GL_RG, GL_HALF_FLOAT, 0, 0, 0, "R16G16_TYPELESS"),
    F(34, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16_SFLOAT, 33, GL_RG16F, GL_RG, GL_HALF_FLOAT, 0, 0, 0, "R16G16_FLOAT"),
    F(35, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16_UNORM, 33, GL_RG16_EXT, GL_RG, GL_UNSIGNED_SHORT, 0, 0, 0, "R16G16_UNORM"),
    F(36, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16G16_UINT, 33, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, "R16G16_UINT"),
    F(37, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16G16_SNORM, 33, GL_RG16_SNORM_EXT, GL_RG, GL_SHORT, 0, 0, 0, "R16G16_SNORM"),
    F(38, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16G16_SINT, 33, GL_RG16I, GL_RG_INTEGER, GL_SHORT, 0, 0, 0, "R16G16_SINT"),
    F(39, T, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32_SFLOAT, 39, GL_R32F, GL_RED, GL_FLOAT, 0, 0, 0, "R32_TYPELESS"),
    F(40, D, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_D32_SFLOAT, 39, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 0, 0, 0, "D32_FLOAT"),
    F(41, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_R32_SFLOAT, 39, GL_R32F, GL_RED, GL_FLOAT, 0, 0, 0, "R32_FLOAT"),
    F(42, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32_UINT, 39, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, "R32_UINT"),
    F(43, C, 4, 1, 0, 0, 0, 1, 0, VK_FORMAT_R32_SINT, 39, GL_R32I, GL_RED_INTEGER, GL_INT, 0, 0, 0, "R32_SINT"),
    F(44, T, 4, 1, 0, 0, 1, 0, 0, VK_FORMAT_D24_UNORM_S8_UINT, 44, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, 0, 0, "R24G8_TYPELESS"),
    F(45, D, 4, 1, 0, 0, 1, 0, 0, VK_FORMAT_D24_UNORM_S8_UINT, 44, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, 0, 0, "D24_UNORM_S8_UINT"),
    F(46, D, 4, 1, 0, 0, 1, 0, 0, VK_FORMAT_D24_UNORM_S8_UINT, 44, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, 0, 0, "R24_UNORM_X8_TYPELESS"),
    F(47, D, 4, 1, 0, 0, 1, 1, 0, VK_FORMAT_D24_UNORM_S8_UINT, 44, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, 0, 0, "X24_TYPELESS_G8_UINT"),
    F(48, T, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8_UNORM, 48, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8_TYPELESS"),
    F(49, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8_UNORM, 48, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8_UNORM"),
    F(50, C, 2, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8G8_UINT, 48, GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, "R8G8_UINT"),
    F(51, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8G8_SNORM, 48, GL_RG8_SNORM, GL_RG, GL_BYTE, 0, 0, 0, "R8G8_SNORM"),
    F(52, C, 2, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8G8_SINT, 48, GL_RG8I, GL_RG_INTEGER, GL_BYTE, 0, 0, 0, "R8G8_SINT"),
    F(53, T, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16_SFLOAT, 53, GL_R16F, GL_RED, GL_HALF_FLOAT, 0, 0, 0, "R16_TYPELESS"),
    F(54, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16_SFLOAT, 53, GL_R16F, GL_RED, GL_HALF_FLOAT, 0, 0, 0, "R16_FLOAT"),
    F(55, D, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_D16_UNORM, 53, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0, 0, 0, "D16_UNORM"),
    F(56, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16_UNORM, 53, GL_R16_EXT, GL_RED, GL_UNSIGNED_SHORT, 0, 0, 0, "R16_UNORM"),
    F(57, C, 2, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16_UINT, 53, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, "R16_UINT"),
    F(58, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R16_SNORM, 53, GL_R16_SNORM_EXT, GL_RED, GL_SHORT, 0, 0, 0, "R16_SNORM"),
    F(59, C, 2, 1, 0, 0, 0, 1, 0, VK_FORMAT_R16_SINT, 53, GL_R16I, GL_RED_INTEGER, GL_SHORT, 0, 0, 0, "R16_SINT"),
    F(60, T, 1, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8_UNORM, 60, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 0, 0, 0, "R8_TYPELESS"),
    F(61, C, 1, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8_UNORM, 60, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 0, 0, 0, "R8_UNORM"),
    F(62, C, 1, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8_UINT, 60, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, "R8_UINT"),
    F(63, C, 1, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8_SNORM, 60, GL_R8_SNORM, GL_RED, GL_BYTE, 0, 0, 0, "R8_SNORM"),
    F(64, C, 1, 1, 0, 0, 0, 1, 0, VK_FORMAT_R8_SINT, 60, GL_R8I, GL_RED_INTEGER, GL_BYTE, 0, 0, 0, "R8_SINT"),
    F(65, C, 1, 1, 0, 0, 0, 0, 0, VK_FORMAT_R8_UNORM, 65, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 0, 0, 1, "A8_UNORM"),
    F(67, C, 4, 1, 0, 0, 0, 0, 0, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 67, GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, 0, 0, 0, "R9G9B9E5_SHAREDEXP"),
    F(70, B, 8, 4, 1, 0, 0, 0, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 70, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, 0, 0, "BC1_TYPELESS"),
    F(71, B, 8, 4, 1, 0, 0, 0, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 70, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, 0, 0, "BC1_UNORM"),
    F(72, B, 8, 4, 1, 1, 0, 0, 0, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 70, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, 0, 0, 0, 0, 0, "BC1_UNORM_SRGB"),
    F(73, B, 16, 4, 2, 0, 0, 0, 0, VK_FORMAT_BC2_UNORM_BLOCK, 73, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, 0, 0, "BC2_TYPELESS"),
    F(74, B, 16, 4, 2, 0, 0, 0, 0, VK_FORMAT_BC2_UNORM_BLOCK, 73, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, 0, 0, "BC2_UNORM"),
    F(75, B, 16, 4, 2, 1, 0, 0, 0, VK_FORMAT_BC2_SRGB_BLOCK, 73, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, 0, 0, 0, 0, 0, "BC2_UNORM_SRGB"),
    F(76, B, 16, 4, 3, 0, 0, 0, 0, VK_FORMAT_BC3_UNORM_BLOCK, 76, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, 0, 0, "BC3_TYPELESS"),
    F(77, B, 16, 4, 3, 0, 0, 0, 0, VK_FORMAT_BC3_UNORM_BLOCK, 76, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, 0, 0, "BC3_UNORM"),
    F(78, B, 16, 4, 3, 1, 0, 0, 0, VK_FORMAT_BC3_SRGB_BLOCK, 76, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, 0, 0, 0, 0, 0, "BC3_UNORM_SRGB"),
    F(79, B, 8, 4, 4, 0, 0, 0, 0, VK_FORMAT_BC4_UNORM_BLOCK, 79, GL_COMPRESSED_RED_RGTC1_EXT, 0, 0, 0, 0, 0, "BC4_TYPELESS"),
    F(80, B, 8, 4, 4, 0, 0, 0, 0, VK_FORMAT_BC4_UNORM_BLOCK, 79, GL_COMPRESSED_RED_RGTC1_EXT, 0, 0, 0, 0, 0, "BC4_UNORM"),
    F(81, B, 8, 4, 4, 0, 0, 0, 0, VK_FORMAT_BC4_SNORM_BLOCK, 79, GL_COMPRESSED_SIGNED_RED_RGTC1_EXT, 0, 0, 0, 0, 0, "BC4_SNORM"),
    F(82, B, 16, 4, 5, 0, 0, 0, 0, VK_FORMAT_BC5_UNORM_BLOCK, 82, GL_COMPRESSED_RED_GREEN_RGTC2_EXT, 0, 0, 0, 0, 0, "BC5_TYPELESS"),
    F(83, B, 16, 4, 5, 0, 0, 0, 0, VK_FORMAT_BC5_UNORM_BLOCK, 82, GL_COMPRESSED_RED_GREEN_RGTC2_EXT, 0, 0, 0, 0, 0, "BC5_UNORM"),
    F(84, B, 16, 4, 5, 0, 0, 0, 0, VK_FORMAT_BC5_SNORM_BLOCK, 82, GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT, 0, 0, 0, 0, 0, "BC5_SNORM"),
    F(85, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_R5G6B5_UNORM_PACK16, 85, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 0, 0, 0, "B5G6R5_UNORM"),
    F(86, C, 2, 1, 0, 0, 0, 0, 0, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 86, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 2, 0, 0, "B5G5R5A1_UNORM"),
    F(87, C, 4, 1, 0, 0, 0, 0, 1, VK_FORMAT_B8G8R8A8_UNORM, 87, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 0, 0, "B8G8R8A8_UNORM"),
    F(88, C, 4, 1, 0, 0, 0, 0, 1, VK_FORMAT_B8G8R8A8_UNORM, 87, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 1, 0, "B8G8R8X8_UNORM"),
    F(90, T, 4, 1, 0, 0, 0, 0, 1, VK_FORMAT_B8G8R8A8_UNORM, 87, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 0, 0, "B8G8R8A8_TYPELESS"),
    F(91, C, 4, 1, 0, 1, 0, 0, 1, VK_FORMAT_B8G8R8A8_SRGB, 87, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 1, 0, 0, "B8G8R8A8_UNORM_SRGB"),
};

const r10_format *r10_format_get(uint32_t dxgi)
{
    static const r10_format *table[128];
    static int ready;
    if (!ready) {
        for (size_t i = 0; i < sizeof formats / sizeof formats[0]; ++i) table[formats[i].dxgi] = &formats[i];
        ready = 1;
    }
    return dxgi < 128 ? table[dxgi] : NULL;
}

uint32_t r10_format_resource_typed(uint32_t dxgi, uint32_t bind)
{
    int depth = (bind & D3D10_BIND_DEPTH_STENCIL) != 0;
    switch (dxgi) {
    case 19: return 20;                          /* R32G8X24_TYPELESS */
    case 39: return depth ? 40 : 41;             /* R32_TYPELESS */
    case 44: return 45;                          /* R24G8_TYPELESS */
    case 53: return depth ? 55 : 54;             /* R16_TYPELESS */
    case 1: return 2;
    case 5: return 6;
    case 9: return 10;
    case 15: return 16;
    case 23: return 24;
    case 27: return 28;
    case 33: return 34;
    case 48: return 49;
    case 60: return 61;
    case 70: return 71;
    case 73: return 74;
    case 76: return 77;
    case 79: return 80;
    case 82: return 83;
    case 90: return 87;
    default: return dxgi;
    }
}

uint32_t r10_format_row_pitch(const r10_format *f, uint32_t width)
{
    if (!f) return 0;
    return f->block > 1 ? ((width + 3) / 4) * f->bytes : width * f->bytes;
}

uint32_t r10_format_rows(const r10_format *f, uint32_t height)
{
    return f && f->block > 1 ? (height + 3) / 4 : height;
}

float r10_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (!exp) {
        if (!man) bits = sign;
        else { exp = 127 - 15 + 1; while (!(man & 0x400)) { man <<= 1; --exp; } bits = sign | (exp << 23) | ((man & 0x3FF) << 13); }
    } else if (exp == 31) bits = sign | 0x7F800000u | (man << 13);
    else bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float f; memcpy(&f, &bits, 4);
    return f;
}

uint16_t r10_float_to_half(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000, exp = (x >> 23) & 0xFF, man = x & 0x7FFFFF;
    if (exp == 0xFF) return (uint16_t)(sign | 0x7C00 | (man ? 0x200 : 0));
    int e = (int)exp - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        man |= 0x800000;
        uint32_t shift = (uint32_t)(14 - e), half = man >> shift, rest = man & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1))) ++half;
        return (uint16_t)(sign | half);
    }
    uint32_t half = sign | ((uint32_t)e << 10) | (man >> 13), rest = man & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;
    return (uint16_t)half;
}

/* ------------------------------------------------------------------ BC decoding */
static void bc_colors(const uint8_t *b, uint8_t c[4][4], int bc1)
{
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    for (int i = 0; i < 2; ++i) {
        uint16_t v = i ? c1 : c0;
        uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, bl = v & 31;
        c[i][0] = (uint8_t)((r << 3) | (r >> 2));
        c[i][1] = (uint8_t)((g << 2) | (g >> 4));
        c[i][2] = (uint8_t)((bl << 3) | (bl >> 2));
        c[i][3] = 255;
    }
    if (!bc1 || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k] + 1) / 3);
            c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k] + 1) / 3);
        }
        c[2][3] = c[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) { c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2); c[3][k] = 0; }
        c[2][3] = 255; c[3][3] = 0;
    }
}

static void bc_alpha_block(const uint8_t *b, uint8_t out[16], int snorm)
{
    int a0 = snorm ? (int8_t)b[0] : b[0], a1 = snorm ? (int8_t)b[1] : b[1];
    int lo = snorm ? -127 : 0, hi = snorm ? 127 : 255;
    if (snorm) { if (a0 == -128) a0 = -127; if (a1 == -128) a1 = -127; }
    int a[8];
    a[0] = a0; a[1] = a1;
    if (a0 > a1) for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    else { for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5; a[6] = lo; a[7] = hi; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i) {
        int v = a[(bits >> (3 * i)) & 7];
        out[i] = (uint8_t)(snorm ? (v + 127) * 255 / 254 : v);   /* SNORM decodes to the unsigned image of -1..1 */
    }
}

void r10_bc_decode(int bc, int snorm, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch,
                   uint8_t *rgba, uint32_t out_pitch)
{
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    uint32_t block_bytes = (bc == 1 || bc == 4) ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t *b = src + by * pitch + bx * block_bytes;
            uint8_t texel[16][4];
            if (bc <= 3) {
                const uint8_t *color = bc == 1 ? b : b + 8;
                uint8_t c[4][4];
                bc_colors(color, c, bc == 1);
                uint32_t idx = (uint32_t)color[4] | (uint32_t)color[5] << 8 | (uint32_t)color[6] << 16 | (uint32_t)color[7] << 24;
                for (int i = 0; i < 16; ++i) memcpy(texel[i], c[(idx >> (2 * i)) & 3], 4);
                if (bc == 2) {
                    for (int i = 0; i < 16; ++i) { uint8_t a = (uint8_t)((b[i / 2] >> (4 * (i & 1))) & 15); texel[i][3] = (uint8_t)(a * 17); }
                } else if (bc == 3) {
                    uint8_t a[16];
                    bc_alpha_block(b, a, 0);
                    for (int i = 0; i < 16; ++i) texel[i][3] = a[i];
                }
            } else {
                uint8_t r[16], g[16];
                bc_alpha_block(b, r, snorm);
                if (bc == 5) bc_alpha_block(b + 8, g, snorm);
                else memset(g, 0, sizeof g);
                for (int i = 0; i < 16; ++i) { texel[i][0] = r[i]; texel[i][1] = g[i]; texel[i][2] = 0; texel[i][3] = 255; }
            }
            for (uint32_t y = 0; y < 4; ++y) {
                uint32_t py = by * 4 + y;
                if (py >= height) break;
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t px = bx * 4 + x;
                    if (px >= width) break;
                    memcpy(rgba + py * out_pitch + px * 4, texel[y * 4 + x], 4);
                }
            }
        }
    }
}
