# Sestavení NXVK pro Tegra X1 a připojení do aplikace

Tento návod platí pro **Nintendo Switch / Horizon / AArch64**, CPU **Cortex-A57**
a GPU **GM20B (Maxwell, SM 5.3)**. Sestavuje **pouze Vulkan/NVK + NAK**, nikoli
OpenGL, Zink, EGL nebo GLES. Takty ani governor nemění.

## 1. Jak to funguje

NXVK se do aplikace **staticky přilinkuje**. Výstupem driveru jsou hlavně:

- `libnvk.a` — Vulkan driver;
- `libnvk_support.a` — sloučené podpůrné knihovny včetně překladače shaderů;
- `nxvk.pc` — příznaky pro kompilaci/linkování přes `pkg-config`;
- hlavičky `vulkan/` a `vk_video/`.

**Nejde o ovladač, který stačí zkopírovat na SD kartu.** Aplikaci musíš znovu
sestavit a prolinkovat. Na SD pak patří výsledné `.nro` aplikace a její data,
ne samostatné `.a` knihovny. Tato varianta používá statický ICD resolver
`vk_icdGetInstanceProcAddr`, nikoli načítání `libvulkan.so` přes `dlopen`.

### Nejrychlejší cesta: již ověřený build č. 7

- [Knihovny, hlavičky a package-linked smoke test](https://github.com/berlint-hub/nxvk/actions/runs/38929697655/artifacts/12058826822)
- [Úspěšné sestavení v devkitPro CI](https://github.com/berlint-hub/nxvk/actions/runs/38929697655)
- [Vyhodnocení kompletního běhu na Switchi](switch/docs/build-7-validation.md)

Driverový commit je `46972d49478d91b3f968d84922fc621ea4ca5c94`.
Tento build prošel **22 PASS, 0 FAIL, 0 GONE, 0 SKIP**. Jde o pokrytí dodanou
regresní sadou, nikoli Vulkan CTS, záruku funkčnosti každé aplikace nebo důkaz
vyšších herních FPS. Artifacts mají omezenou dobu uchování; archiv a jeho checksum
si ulož. Pokud odkaz později expiruje, použij sestavení níže.

### Nový integrační balík s hotovým příkladem

**Hotový integrační build č. 10:**
[stáhnout SDK a samostatnou aplikaci](https://github.com/berlint-hub/nxvk/actions/runs/38943461562/artifacts/12064098920).
[CI běh](https://github.com/berlint-hub/nxvk/actions/runs/38943461562) pro commit
`623323bebc559a61275436a01cdbd5e43095abd5` úspěšně sestavil celý Vulkan balík i
samostatnou CMake aplikaci z přemístěné distribuce. Ověřil AArch64, původ obou
archivů v link mapě a absenci test runneru. Nová aplikace zatím nebyla ověřena
spuštěním na konzoli a NetherSX2 tím nebyl automaticky integrován.

Nové běhy workflow přidávají artifact **`nxvk-app-integration-<číslo běhu>`**.
Obsahuje stejný Vulkan tarball, jeho checksum a samostatné
`nxvk_app_example.nro` s checksumem a údaji o driveru. V tarballu navíc najdeš:

- `include/nxvk/nxvk_static.h` — deklaraci statického resolveru pro C/C++;
- `lib/cmake/NXVK/NXVKConfig.cmake` — hotový target `NXVK::Driver`;
- `examples/switch-minimal/` — zdroje samostatné aplikace a build skript;
- `INTEGRATION.md` — návod a hranice dodané kompatibility.

Aktuální běhy a artifacts najdeš v [Actions](https://github.com/berlint-hub/nxvk/actions).
Příklad jde sestavit pouze z rozbaleného balíku a devkitPro SDK; nepotřebuje zdroje
Mesa, Rust ani nástroje pro generování shaderů. Starší ověřený build č. 7 tyto
nové integrační soubory ještě nemá. Kód optimalizovaného driveru se při této
úpravě balení nemění, ale nově sestavené NRO není automaticky hardware-testované.

## 2. Co potřebuješ pro vlastní sestavení

Ověřená cesta je Linux x86-64 (CI používá Ubuntu 24.04) s:

- Git, GNU Make a Bash;
- Dockerem a přístupem k jeho daemonu;
- internetem pro první vytvoření toolchain image.

DevkitPro, libnx, portlibs, Rust a LLVM nemusíš instalovat na hosta. Vytvoří je
projektový [`switch/docker/Dockerfile`](switch/docker/Dockerfile). Ten používá
`devkitpro/devkita64`, LLVM 15 a Rust `nightly-2026-07-02` s `rust-src`.
Switch portlibs jsou už v základním image; není potřeba kvůli NXVK odinstalovávat
`switch-mesa` ani znovu instalovat všechny Switch balíčky přes pacman.

Příkazy spouštěj z kořene NXVK checkoutu. Pro jednoduché převzetí příznaků
`pkg-config` do shellových polí používej cesty bez mezer.

### Správná větev

Optimalizace této práce jsou ve větvi **`arena/068e6f2f-nxvk`**. Samotná starší
větev `switch` není náhradou za tuto optimalizovanou větev.

Pro nový checkout, v adresáři, kde ještě neexistuje složka `nxvk`:

```bash
git clone --single-branch --branch arena/068e6f2f-nxvk https://github.com/berlint-hub/nxvk.git nxvk
cd nxvk
git branch --show-current
git log -1 --oneline
```

V již otevřeném správném checkoutu stačí poslední dva kontrolní příkazy.
Nepřepisuj si přitom neuložené změny aplikace ani jiné pracovní větve.

## 3. Sestavení optimalizovaných Vulkan knihoven

```bash
make CONTAINER=docker image
make CONTAINER=docker all
```

`all` sestaví nativní generovací nástroje, Rust sysroot, Vulkan driver a sloučené
archivy. Nepoužívej `make gl`, `make install-gl` ani `configure-zink.sh`, pokud
chceš tuto Vulkan-only variantu. Výchozí hodnota `CONTAINER` v Makefile je Podman,
proto je zde Docker uveden výslovně.

Výstup `make all`:

```text
switch/build/pkg/lib/libnvk.a
switch/build/pkg/lib/libnvk_support.a
switch/build/pkg/lib/pkgconfig/nxvk.pc
```

**Pozor:** `make all` samo nepřibalí hlavičky, licenci a `BUILDINFO.txt` a nevytvoří
distribuční tarball. To provádí následující krok, případně přímo CI workflow.

### Optimalizace, které jsou již zapnuté

Není potřeba dodatečný „Tegra performance“ přepínač:

| Oblast | Nastavení / změna |
| --- | --- |
| C/C++ driver | release, `-O3`, `NDEBUG`, `-march=armv8-a+crc+crypto`, `-mtune=cortex-a57` |
| Rust driver a sysroot | `cortex-a57`; cílení se vztahuje i na sestavované `core`, `alloc`, `std` |
| TLS a linkování | Switch ABI, `-mtp=soft`, lokální TLS model, samostatné funkční/datové sekce |
| GPU shadery | NAK vybírá SM podle GPU; CPU přepínač Cortex-A57 není přepínač shaderové architektury |
| Interní uncached mapy | vynechání nepotřebného atomického sledování dirty rozsahů při zachování publikace zápisů |
| Shaderová cache | identický klíč a obsah se zbytečně znovu nezapisuje na SD; nestačí pouhá shoda CRC |
| Zachované cesty | T210 subtiling, NVMap reuse, zero-copy prezentace, komprese, ZCULL a potřebná synchronizace |

Nezapínej automaticky `-Ofast`, `-ffast-math` nebo LTO pro celý projekt.
Příznaky driveru jsou v [`configure-mesa.sh`](switch/build/configure-mesa.sh)
a [`switch.cross`](switch/crossfiles/switch.cross). Testovací aplikace mají vlastní
kompilační nastavení; tvrzení o `-O3` zde platí pro driver, ne libovolnou aplikaci.

Podrobnosti, zdroje a rozsah ověření:
[optimalizační rozbor](switch/docs/tegra-x1-optimization.md) a
[předchozí audit](switch/docs/tegra-x1-audit.md).

## 4. Vytvoření kompletního distribučního balíku

Po úspěšném `make all` spusť tento blok z kořene NXVK. Mezi kompilací a balením
neměň zdroje ani commit. U staženého buildu zachovej jeho původní metadata;
nenahrazuj je údajem o jiném checkoutu. Balení běží uvnitř image,
aby nevznikaly problémy s oprávněním k souborům vytvořeným kontejnerem.

```bash
set -euo pipefail
IMAGE_ID="$(docker image inspect nxvk --format '{{.Id}}')"
docker run --rm -v "$PWD:/work:z" -w /work \
  -e NXVK_TOOLCHAIN_IMAGE_ID="$IMAGE_ID" nxvk bash -euo pipefail -c '
  pkg=switch/build/pkg
  mkdir -p "$pkg/include" "$pkg/licenses/nxvk" switch/build/logs
  test -s "$pkg/lib/libnvk.a"
  test -s "$pkg/lib/libnvk_support.a"
  test -s "$pkg/lib/pkgconfig/nxvk.pc"
  test ! -e "$pkg/lib/libnvk_gl.a"
  cp -r include/vulkan include/vk_video "$pkg/include/"
  cp licenses/GPL-2.0-or-later "$pkg/licenses/nxvk/"
  {
    printf "NXVK Vulkan-only / Nintendo Switch Tegra X1\n"
    printf "Source: berlint-hub/nxvk\n"
    printf "Commit: %s\n" "$(git -c safe.directory=/work rev-parse HEAD)"
    printf "Ref: %s\n" "$(git -c safe.directory=/work symbolic-ref HEAD)"
    printf "Build run: local\nToolchain image: %s\n" "$NXVK_TOOLCHAIN_IMAGE_ID"
    printf "Mesa version: "; cat VERSION
    if test -n "$(git -c safe.directory=/work status --porcelain)"; then
      printf "Worktree at packaging: dirty\n"
    else
      printf "Worktree at packaging: clean\n"
    fi
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc --version
    rustc --version
    cargo --version
    meson --version
    ninja --version
    dkp-pacman -Q
  } > switch/build/logs/BUILDINFO.txt
  cp switch/build/logs/BUILDINFO.txt "$pkg/BUILDINFO.txt"
  cp switch/docs/tegra-x1-audit.md "$pkg/TEGRA-X1-AUDIT.md"
  cp switch/docs/tegra-x1-optimization.md "$pkg/TEGRA-X1-OPTIMIZATION.md"
  bash switch/integration/stage.sh "$pkg"
  tar -czf switch/build/nxvk-vulkan-tegra-x1.tar.gz -C "$pkg" .
  cd switch/build
  sha256sum nxvk-vulkan-tegra-x1.tar.gz > nxvk-vulkan-tegra-x1.tar.gz.sha256
'
```

Výsledné soubory:

```text
switch/build/nxvk-vulkan-tegra-x1.tar.gz
switch/build/nxvk-vulkan-tegra-x1.tar.gz.sha256
```

Image má některé plovoucí závislosti. Nové sestavení proto není automaticky
bitově shodné s buildem č. 7; uchovej jeho `BUILDINFO.txt` a checksum.
Při změně toolchainu raději proveď čistý rebuild. Rust target JSON/sysroot skript
a configure soubory mají potřebné závislosti v Makefile, ale změna image sama
není úplnou zárukou invalidace všech starých výstupů.

Volitelné vyčištění **generovaných výstupů** před opakováním sestavení:

```bash
docker run --rm -v "$PWD:/work:z" -w /work nxvk bash -euc 'make CONTAINER= distclean'
make CONTAINER=docker all
```

## 5. Ověření knihoven a sestavení testů

### Klient linkovaný pouze z rozbaleného balíku

```bash
docker run --rm -v "$PWD:/work:z" -w /work nxvk \
  bash switch/build/verify-package.sh switch/build/nxvk-vulkan-tegra-x1.tar.gz
```

Tento skript ověří checksum, rozbalí balík do dočasné složky a sestaví skutečného
klienta přes jeho `nxvk.pc`. Kontroluje AArch64 ELF, ICD symbol a použití obou
archivů z distribuce místo build-tree knihoven. Zdroj klienta a testovací
kompatibilitu bere z tohoto repozitáře. Výstup je
`switch/build/package-check/nvk_smoke.nro`. **Je to kontrola kompilace/linkování,
ne spuštění GPU testu.**

### Samostatná aplikace sestavená pouze z distribuce

```bash
docker run --rm -v "$PWD:/work:z" -w /work nxvk \
  bash switch/integration/verify.sh switch/build/nxvk-vulkan-tegra-x1.tar.gz
```

Ověřovač přesune rozbalený balík do jiné cesty (záměrně i s mezerou), sestaví
přiložený CMake projekt, zkontroluje AArch64 ELF a oba archivy v link mapě.
Ověřuje také nepřítomnost cest test runneru. Výstup:
`switch/build/integration-check/nxvk_app_example.nro`.
Jde o nové sestavení/linkování, ne o spuštění na konzoli.

### Celá Vulkan testovací sada

```bash
make CONTAINER=docker vulkan-tests
```

Výstup je `switch/smoke/out/vulkan/`: 22 aplikací a `nvk_runner.nro`.
Tento příkaz je sestaví, ale sám nevytvoří celý SD tarball s metadaty; hotový
SD balík publikuje CI. Postup instalace a sběru logů je v
[VULKAN-TESTING.md](switch/smoke/VULKAN-TESTING.md).

Nativní regresní testy lze spustit i bez SDK; potřebují C a C++ kompilátor, ASan/UBSan,
Python 3, Git a GNU Make:

```bash
bash switch/tests/run-host-tests.sh
```

Zvláštní měřicí balík, pokud ho potřebuješ:

```bash
make CONTAINER=docker vulkan-diagnostics
```

Ten už vytváří `switch/build/nxvk-vulkan-diagnostics.tar.gz`; jeho dvě diagnostiky
nenahrazují celou 22testovou sadu. Návod je v
[diagnostics/README.md](switch/diagnostics/README.md).

## 6. Připojení knihoven do vlastní aplikace

Používej **obě `.a` knihovny ze stejného sestavení** a přiložené Vulkan hlavičky.
Výsledná aplikace musí být sestavována pomocí devkitA64/libnx pro Horizon,
ne hostitelským x86 kompilátorem. Pro pouhé linkování hotových knihoven nepotřebuješ
znovu sestavovat Rust ani Mesa; potřebuješ kompatibilní devkitPro prostředí.

### 6.0 Zkus hotový samostatný projekt

Z kořene **rozbaleného nového balíku**, v devkitPro prostředí s CMake 3.16+:

```bash
bash examples/switch-minimal/build.sh
```

Vznikne `examples/switch-minimal/out/nxvk_app_example.nro`. Aplikace vytvoří
Vulkan instanci/zařízení, provede GPU fill a ověří 1 024 slov. Loguje do
`sdmc:/nxvk_app_example.log` a čeká na `+`. Není to renderer s oknem ani benchmark.
Spouštěj ji v plné paměti přes title override. Zdroj obsahuje vlastní startup
globály, takže ho nekopíruj celý do projektu, který už má svůj `main` a init.

Build wrapper explicitně povoluje omezený `example_compat.c` dodaný v příkladu.
Pro skutečnou aplikaci platí upozornění v části 6.3; CMake target driveru sám
žádné takové globální náhrady nepřidává.

### 6.1 Soukromý prefix v aplikaci — doporučeno

Následující příkazy jsou již z kořene **tvé aplikace**, kam jsi přinesl tarball
a sousední `.sha256` soubor:

```bash
set -euo pipefail
sha256sum -c nxvk-vulkan-tegra-x1.tar.gz.sha256
mkdir -p third_party/nxvk
tar -xzf nxvk-vulkan-tegra-x1.tar.gz -C third_party/nxvk
NXVK_PREFIX="$(realpath third_party/nxvk)"
```

Dostaneš `third_party/nxvk/lib`, `include`, `licenses` a `BUILDINFO.txt`.
Nepřepisuješ tak globální SDK ani závislosti jiných aplikací. Nemíchej tento
Vulkan driver s `libnvk_gl.a` nebo jinou implementací Vulkanu ve stejném rendereru.
Jiné závislosti celé aplikace/launcheru nejsou tímto návodem rušeny.

### 6.2 `pkg-config` a ověřené pořadí linkeru

Starší `nxvk.pc` z buildu č. 7 má prefix pro instalaci do devkitPro portlibs.
Při rozbalení do vlastní složky ho **přepiš argumentem `--define-variable=prefix=...`**.
Nové integrační balíky už používají přemístitelný prefix podle `pcfiledir`.
Následující explicitní přepsání funguje s oběma variantami.

```bash
set -euo pipefail
: "${DEVKITPRO:?Nastav DEVKITPRO na svou instalaci devkitPro}"
NXVK_PREFIX="$(realpath third_party/nxvk)"
NXVK_CFLAGS_TEXT="$(PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= \
  PKG_CONFIG_LIBDIR="$NXVK_PREFIX/lib/pkgconfig" \
  pkg-config --define-variable=prefix="$NXVK_PREFIX" --cflags nxvk)"
NXVK_LIBS_TEXT="$(PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR= \
  PKG_CONFIG_LIBDIR="$NXVK_PREFIX/lib/pkgconfig" \
  pkg-config --define-variable=prefix="$NXVK_PREFIX" --libs nxvk)"
read -r -a NXVK_CFLAGS <<< "$NXVK_CFLAGS_TEXT"
read -r -a NXVK_LIBS <<< "$NXVK_LIBS_TEXT"
printf 'Cflags: %s\nLibs: %s\n' "$NXVK_CFLAGS_TEXT" "$NXVK_LIBS_TEXT"
```

Do kompilace vlastních zdrojů přidej `"${NXVK_CFLAGS[@]}"`, libnx include cestu
a Switch ABI příznaky používané tvým projektem. `pkg-config` zde nezasahuje do
vyhledávání ostatních závislostí celé aplikace: prostředí je nastavené pouze pro
tyto dva dotazy.

Níže je **šablona linkovacího kroku**, ne samostatný build aplikace: `app.o` a
`app_platform.o` nahraď jejími již sestavenými objektovými soubory. Druhý název
představuje její platformní kompatibilitu popsanou v následující části.

```bash
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
ARCH=(-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE)
"$CXX" -specs="$DEVKITPRO/libnx/switch.specs" "${ARCH[@]}" \
  -o app.elf app.o app_platform.o \
  "${NXVK_LIBS[@]}" \
  -L"$DEVKITPRO/portlibs/switch/lib" -L"$DEVKITPRO/libnx/lib" \
  -lnx -lc -lm -lstdc++ -pthread -Wl,-Map=app.map
```

Tvar vychází z funkčního [`verify-package.sh`](switch/build/verify-package.sh).
`nxvk.pc` už obsahuje potřebné `--whole-archive` pro NVK, skupinu podpůrných
archivů, `-lz`, `-lexpat`, zachování ICD symbolu a `--gc-sections`. Neodstraňuj je
ani nepřidávej tentýž celý NVK archiv pod `--whole-archive` podruhé. ELF následně
zabal do NRO standardním postupem své aplikace (`nacptool`/`elf2nro`).

### 6.3 Kompatibilita newlib/libnx — důležité

**Dvojice knihoven není náhradou za celý runtime aplikace.** Ověřený testovací
klient navíc linkuje [`switch/smoke/nvk_compat.c`](switch/smoke/nvk_compat.c),
který doplňuje chybějící POSIX/newlib symboly, například `getrandom`,
`posix_memalign`, `sysconf`, `pthread_sigmask`, `reg*`, `flock`, `dirfd`, `fstatat`
a `getpwuid_r`.

- Má-li aplikace vlastní Horizon kompatibilní vrstvu, použij a sjednoť ji;
  neduplikuj globální definice těchto funkcí.
- Testovací `nvk_compat.c` **není obecná produkční implementace POSIX**: regex
  náhrada nikdy nenajde shodu, `flock` skutečně nezamyká a paměťový údaj v
  `sysconf` je zjednodušený. Nepřebírej jej bez kontroly do celého emulátoru.
- U chyb typu `undefined reference` nejdřív rozliš chybějící SDK knihovnu od
  chybějící platformní funkce. Nedoplňuj libovolné funkce vracející úspěch jen
  proto, aby linker přestal hlásit chyby.
- Nepoužívej `--allow-multiple-definition` k zakrytí konfliktu kompatibilních
  implementací.
- Do skutečné aplikace nekopíruj `nvk_chain.c` ani `nvk_harness.h`. První vrací
  řízení test runneru, druhý obsahuje vlastní testovací init/globální nastavení.

Balík č. 7 neobsahuje samostatný `libnxvk_compat.a` ani hotovou úpravu NetherSX2.
Kompatibilitu runtime a loader rendereru je potřeba zapojit v cílové aplikaci.

### 6.4 Hotový target pro existující CMake projekt

V novém integračním balíku nastav `NXVK_DIR` na absolutní cestu
`third_party/nxvk/lib/cmake/NXVK`, ponech vlastní Horizon toolchain/startup specs
a po vytvoření targetu aplikace použij:

```cmake
find_package(NXVK CONFIG REQUIRED)
target_include_directories(my_app BEFORE PRIVATE "${NXVK_INCLUDE_DIR}")
target_link_libraries(my_app PRIVATE NXVK::Driver)
```

Target dodává linkerové skupiny, knihovny, SDK search paths a zachování ICD
symbolu, ale nenastavuje tvůj `main`, applet mode, linker script ani runtime
kompatibilitu. Použij C++ linker. Přiložený standalone projekt má vlastní malý
toolchain; nepřidávej ho navrch už funkčního toolchainu emulátoru.

Následující ruční varianta zůstává pro starší balík č. 7. **Nekombinuj ji s
`NXVK::Driver` ve stejném targetu.**

Pokud už máš funkční devkitPro CMake projekt, můžeš místo `pkg-config` explicitně
připojit stejné knihovny. **Toto není kompletní CMakeLists ani toolchain:** nahraď
`my_app` existujícím targetem aplikace, nastav její Switch toolchain a povol
v projektu jazyk C++. Používá se C++ linker. Neaplikuj zároveň tento blok i druhou kopii stejného linkování z `.pc`.

```cmake
set(NXVK_ROOT "${CMAKE_SOURCE_DIR}/third_party/nxvk" CACHE PATH "Rozbaleny NXVK balik")
set_property(TARGET my_app PROPERTY LINKER_LANGUAGE CXX)

target_include_directories(my_app BEFORE PRIVATE
    "${NXVK_ROOT}/include"
    "$ENV{DEVKITPRO}/libnx/include")
target_compile_definitions(my_app PRIVATE __SWITCH__ VK_USE_PLATFORM_VI_NN)
target_compile_options(my_app PRIVATE -ffunction-sections -fdata-sections)
target_link_directories(my_app PRIVATE
    "$ENV{DEVKITPRO}/portlibs/switch/lib"
    "$ENV{DEVKITPRO}/libnx/lib")
target_link_options(my_app PRIVATE
    "-specs=$ENV{DEVKITPRO}/libnx/switch.specs"
    "-Wl,-u,vk_icdGetInstanceProcAddr"
    "-Wl,--gc-sections")
target_link_libraries(my_app PRIVATE
    "-Wl,--whole-archive" "${NXVK_ROOT}/lib/libnvk.a" "-Wl,--no-whole-archive"
    "-Wl,--start-group" "${NXVK_ROOT}/lib/libnvk_support.a"
    z expat nx c m stdc++ "-pthread" "-Wl,--end-group")
```

Přesné zapojení do CMake cílového emulátoru zde nebylo sestaveno/ověřeno.
CI zvlášť staví dodaný standalone CMake projekt z rozbaleného balíku; to není
záruka kompatibility se všemi build pravidly jiné aplikace. Kompatibilní funkce
z části 6.3 musí zajistit aplikace i při použití CMake.

## 7. Inicializace Vulkanu v aplikaci

### Statický resolver a povinný opt-in pro GM20B

Před vytvořením instance musí aplikace nastavit
`NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`. Je to explicitní povolení necertifikovaného
GM20B targetu. Bez něj může release driver vrátit nula fyzických zařízení.
Testovací harness jej nastavuje, vlastní aplikace to musí udělat sama.

Příklad bootstrap funkce pro C/C++ (bez test runneru a bez okna):

```c
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_VI_NN
#define VK_USE_PLATFORM_VI_NN
#endif
#include <vulkan/vulkan.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
#ifdef __cplusplus
}
#endif

VkResult nxvk_create_instance(VkInstance *out)
{
    if (out == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    if (setenv("NVK_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 1) != 0)
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)
        vk_icdGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (create_instance == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "my_app";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    return create_instance(&ci, NULL, out);
}
```

Další funkce načítej přes instance/device proc-address funkce, ověř jejich
ukazatele a návratové kódy a při ukončení znič vytvořené Vulkan objekty.
`VK_NO_PROTOTYPES` patří před první zahrnutí Vulkan hlaviček; neřeš chybějící
přímý `vkCreateInstance` přilinkováním náhodného desktopového Vulkan loaderu.

Pokud používáš **volk**, použij statickou/custom inicializaci:
`volkInitializeCustom(vk_icdGetInstanceProcAddr)`, potom po vytvoření instance
`volkLoadInstance(instance)` a po vytvoření zařízení `volkLoadDevice(device)`
(nebo device table pro více zařízení). Povinný opt-in stále nastav před instancí.
Volk není součástí tohoto balíku; nepřidávej druhou kopii, pokud jej aplikace má.

### Obraz na displeji

Výše uvedený příklad je headless. Pro prezentaci přidej instance extensions
`VK_KHR_surface` a `VK_NN_vi_surface`, device extension `VK_KHR_swapchain`,
ověř podporu prezentace vybrané queue family a vytvoř VI surface s platným
libnx `NWindow`, například `nwindowGetDefault()`.

Funkční vzory jsou [`nvk_vi_swapchain.c`](switch/smoke/nvk_vi_swapchain.c)
a [`nvk_present.c`](switch/smoke/nvk_present.c). Win32/Xlib/Wayland surface ani
pouhé `SDL_Vulkan_LoadLibrary` nenahrazují toto Horizon zapojení.

## 8. Běžné chyby a praktické shrnutí

| Problém | Co ověřit |
| --- | --- |
| `undefined reference to vkCreateInstance` | Používáš funkce načtené přes statický ICD resolver / custom Vulkan loader? |
| `undefined reference to vk_icdGetInstanceProcAddr` | Linkuje se správné `libnvk.a`, se zachovaným symbolem a bez záměny za hostitelskou knihovnu? |
| Chybějí POSIX symboly | Doplnit/sjednotit skutečnou runtime kompatibilitu aplikace, viz 6.3 |
| `multiple definition` | Duplicitní compat funkce nebo dvojí `--whole-archive` linkování |
| Balík je rozbalený, ale bere se jiná knihovna | Přepsat prefix `.pc`, použít správné include/link cesty a zkontrolovat `app.map` |
| Nula Vulkan zařízení | Opt-in pro GM20B před instancí, správný Horizon build a návratové kódy inicializace |
| Starý sysroot / jiný toolchain | Čistý rebuild a nové `BUILDINFO.txt`; nemíchat archivy z různých sestavení |
| Nový runner odmítne pokračovat | Zálohovat výsledky a odstranit pouze starý soubor `state`, ne slepě celé logy/cache |

Pro normální provoz nenechávej omylem zapnuté diagnostické varianty
`track_uncached_dirty`, `full_arena_flush`, `full_cmd_flush`, `no_t210_subtiling`
nebo `MESA_SHADER_CACHE_DISABLE=1`. Jejich účelem jsou srovnávací testy, nikoli
zapnutí všech optimalizací. Nevypínej potřebné cache operace ani synchronizaci.

Pro `nethersx2_nx` je potřeba v **jeho** build systému zapojit tento prefix,
knihovny, runtime kompatibilitu a statický Vulkan resolver. Tento repozitář
neobsahuje jeho aktuální build skripty, proto zde není neověřený příkaz, který
by předstíral kompletní sestavení emulátoru. Nejprve ověř link/init driveru,
potom surface/present cestu a až následně renderer v reálné zátěži.

Při distribuci aplikace zohledni licenci portu: balík obsahuje
[`GPL-2.0-or-later`](licenses/GPL-2.0-or-later) části a příslušné licenční povinnosti.
Nepředpokládej, že celý tento fork je pouze pod MIT licencí.
