class_name GlobalHeightmapCache
extends RefCounted
## Persistent whole-planet elevation package used by the spherical clipmap.
##
## Six RF cube faces plus their full mip chains are compressed into one ZSTD file.
## Runtime loads/decompresses this package once when the world is adopted; there is
## no page table, no per-region disk I/O and no terrain residency state afterwards.

const MAGIC: int = 0x4147484D # "AGHM"
const FORMAT_VERSION: int = 1
const FACE_COUNT: int = 6
const IMAGE_FORMAT: Image.Format = Image.FORMAT_RF
const CACHE_ROOT: String = "user://global_heightmaps"


static func path_for(cfg: GenConfig, face_res: int, tex_res: int,
		variant_key: String = "base") -> String:
	var safe_variant: String = variant_key if not variant_key.is_empty() else "base"
	return "%s/%s_%s_v%d_%d_%d.aghm" % [
		CACHE_ROOT,
		cfg.cache_key(),
		safe_variant,
		FORMAT_VERSION,
		face_res,
		tex_res,
	]


static func load_images(cfg: GenConfig, face_res: int, tex_res: int,
		variant_key: String = "base") -> Dictionary:
	var path: String = path_for(cfg, face_res, tex_res, variant_key)
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	if int(file.get_32()) != MAGIC or int(file.get_32()) != FORMAT_VERSION:
		return {}
	if int(file.get_32()) != FACE_COUNT:
		return {}
	if int(file.get_32()) != face_res or int(file.get_32()) != tex_res:
		return {}
	if int(file.get_32()) != int(IMAGE_FORMAT):
		return {}

	var images: Array[Image] = []
	var compressed_total: int = 0
	var raw_total: int = 0
	for _face: int in FACE_COUNT:
		var raw_size: int = int(file.get_64())
		var compressed_size: int = int(file.get_64())
		if raw_size <= 0 or compressed_size <= 0:
			return {}
		var compressed: PackedByteArray = file.get_buffer(compressed_size)
		if compressed.size() != compressed_size:
			return {}
		var raw: PackedByteArray = compressed.decompress(raw_size, FileAccess.COMPRESSION_ZSTD)
		if raw.size() != raw_size:
			return {}
		var image: Image = Image.create_from_data(
			tex_res, tex_res, true, IMAGE_FORMAT, raw)
		if image == null or image.is_empty():
			return {}
		images.append(image)
		compressed_total += compressed_size
		raw_total += raw_size

	return {
		"images": images,
		"path": path,
		"compressed_bytes": compressed_total,
		"raw_bytes": raw_total,
	}


static func save_images(cfg: GenConfig, face_res: int, tex_res: int,
		images: Array[Image], variant_key: String = "base") -> Dictionary:
	if images.size() != FACE_COUNT:
		return {}
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(CACHE_ROOT))
	var path: String = path_for(cfg, face_res, tex_res, variant_key)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return {}

	file.store_32(MAGIC)
	file.store_32(FORMAT_VERSION)
	file.store_32(FACE_COUNT)
	file.store_32(face_res)
	file.store_32(tex_res)
	file.store_32(int(IMAGE_FORMAT))

	var compressed_total: int = 0
	var raw_total: int = 0
	for image: Image in images:
		if image == null or image.is_empty() or image.get_width() != tex_res \
				or image.get_height() != tex_res or image.get_format() != IMAGE_FORMAT \
				or not image.has_mipmaps():
			return {}
		var raw: PackedByteArray = image.get_data()
		var compressed: PackedByteArray = raw.compress(FileAccess.COMPRESSION_ZSTD)
		if compressed.is_empty():
			return {}
		file.store_64(raw.size())
		file.store_64(compressed.size())
		file.store_buffer(compressed)
		raw_total += raw.size()
		compressed_total += compressed.size()

	file.flush()
	return {
		"path": path,
		"compressed_bytes": compressed_total,
		"raw_bytes": raw_total,
	}
