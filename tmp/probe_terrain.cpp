// Probe: print the heightmap texture format of a client map's terrain.
#include <unreal/PackageLoader.h>
#include <unreal/Terrain.h>

#include <iostream>

int main(int argc, char **argv) {
  if (argc < 3)
    return 2;
  unreal::PackageLoader loader{argv[1],
                               {unreal::SearchConfig{"Maps", "unr"},
                                unreal::SearchConfig{"Textures", "utx"},
                                unreal::SearchConfig{"SysTextures", "utx"}}};
  const auto package = loader.load_package(argv[2]);
  if (!package)
    return 3;
  std::vector<std::shared_ptr<unreal::TerrainInfoActor>> terrains;
  package->load_objects("TerrainInfo", terrains);
  std::cout << "terrains=" << terrains.size() << '\n';
  for (const auto &terrain : terrains) {
    if (!terrain || !terrain->terrain_map) {
      std::cout << "terrain without heightmap\n";
      continue;
    }
    const auto texture = terrain->terrain_map.as<unreal::Texture>();
    if (!texture) {
      std::cout << "heightmap did not resolve to a texture\n";
      continue;
    }
    std::cout << "format=0x" << std::hex << int(texture->format) << std::dec
              << " size=" << texture->u_size << "x" << texture->v_size
              << " mips=" << texture->mips.size()
              << " bytes=" << texture->mips[0].data.size() << '\n';
  }
  return 0;
}
