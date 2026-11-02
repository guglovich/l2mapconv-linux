// Probe: mimic VisualSceneLoader's terrain resolution path.
#include <unreal/ArchiveLoader.h>
#include <unreal/Terrain.h>

#include <iostream>

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  unreal::ArchiveLoader loader{argv[1],
                               {unreal::SearchConfig{"Maps", "unr"},
                                unreal::SearchConfig{"Textures", "utx"},
                                unreal::SearchConfig{"SysTextures", "utx"},
                                unreal::SearchConfig{"StaticMeshes", "usx"}}};
  auto *archive = loader.load_archive(argv[2]);
  if (!archive)
    return 3;
  for (auto &e : archive->export_map)
    if (e.class_name == "TerrainInfo") {
      std::cout << "export: class=" << e.class_name << " name=" << e.object_name
                << '\n';
      const auto terrain = std::dynamic_pointer_cast<unreal::TerrainInfoActor>(
          archive->object_loader.export_object(e));
      if (!terrain) {
        std::cout << "cast failed\n";
        continue;
      }
      std::cout << "has_reference=" << terrain->terrain_map.has_reference()
                << " bool=" << (terrain->terrain_map ? 1 : 0) << '\n';
      const auto texture = terrain->terrain_map.as<unreal::Texture>();
      if (!texture) {
        std::cout << "texture null; untyped type="
                  << (terrain->terrain_map.untyped()
                          ? std::string(terrain->terrain_map.untyped()->name)
                           : std::string{"null"})
                  << '\n';
        continue;
      }
      std::cout << "format=0x" << std::hex << int(texture->format) << std::dec
                << " size=" << texture->u_size << "x" << texture->v_size
                << '\n';
      break;
    }
  return 0;
}
