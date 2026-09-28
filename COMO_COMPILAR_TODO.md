# Guía Completa de Compilación: Korsh Core v0.0.4 (Con Nuevo Diseño Integrado)
# Full Compilation Guide: Korsh Core v0.0.4 (With New Design Integrated)

Este paquete contiene todo el código fuente de Korsh Core con el nuevo diseño profesional **Obsidian Luxor y Verde Neón (#00CC52)** ya aplicado en los archivos de Qt y las conexiones reales de la red verificadas.

---

## 1. Conexiones y Parámetros Reales Verificados

* **Red:** Korsh Mainnet (Protocolo 70208)
* **Nodo Semilla Oficial:** `195.26.244.209:9777`
* **Puerto P2P:** `9777 / TCP`
* **Puerto RPC:** `9776 / TCP`
* **Puerto Tor Onion:** `9775 / TCP`
* **Prefijo de Dirección:** Inicia con `K` (Base58: 45)

---

## 2. Dónde están los Archivos del Nuevo Diseño

* **Stylesheet Principal Qt:** `src/qt/res/css/dark.css` (Paleta Luxor Obsidian y Verde Neón).
* **Configuración del Motor Qt:** `src/qt/guiutil.cpp` (Tema Dark por defecto, paleta `#00CC52`).
* **Logotipos de Barra de Herramientas:** `src/qt/res/images/korsh_logo_toolbar.png` y `korsh_logo_toolbar_blue.png`.
* **Icono de la Billetera:** `src/qt/res/icons/korsh.png`.
* **Carpeta de Recursos Web y Mockups:** `wallet_new_design/` (contiene maquetas, simulador HTML interactivo y logos PNG en alta resolución).

---

## 3. Instrucciones de Compilación por Sistema Operativo

### A. Cómo Compilar para WINDOWS (x86_64) usando MSYS2 MinGW-w64

1. Instala **MSYS2** (desde https://www.msys2.org) o abre tu consola **MSYS2 MINGW64** (`C:\msys64\mingw64.exe`).
2. Instala las dependencias necesarias ejecutando en la consola de MSYS2:
   ```bash
   pacman -Syu --needed base-devel git autoconf automake libtool make pkg-config python \
       mingw-w64-x86_64-gcc \
       mingw-w64-x86_64-boost \
       mingw-w64-x86_64-db \
       mingw-w64-x86_64-openssl \
       mingw-w64-x86_64-libevent \
       mingw-w64-x86_64-zeromq \
       mingw-w64-x86_64-sqlite3 \
       mingw-w64-x86_64-miniupnpc \
       mingw-w64-x86_64-libnatpmp \
       mingw-w64-x86_64-qt5-static \
       mingw-w64-x86_64-freetype \
       mingw-w64-x86_64-harfbuzz \
       mingw-w64-x86_64-zstd
   ```
3. Navega a la carpeta de Korsh Core:
   ```bash
   cd /c/Users/lewis/Documents/Korsh
   ```
4. Genera los scripts de configuración:
   ```bash
   ./autogen.sh
   ```
5. Configura el build estático con Qt5 y Berkeley DB:
   ```bash
   export PKG_CONFIG_PATH=/mingw64/qt5-static/lib/pkgconfig:/mingw64/lib/pkgconfig
   export PATH=/mingw64/qt5-static/bin:$PATH
   export PKG_CONFIG_SYSTEM_INCLUDE_PATH=/mingw64/include

   ./configure \
     --disable-bench \
     --disable-tests \
     --disable-gui-tests \
     --disable-stacktraces \
     --disable-zmq \
     --with-gui=qt5 \
     --with-qt-bindir=/mingw64/qt5-static/bin \
     --with-qt-incdir=/mingw64/qt5-static/include \
     --with-qt-libdir=/mingw64/qt5-static/lib \
     --with-qt-plugindir=/mingw64/qt5-static/share/qt5/plugins \
     --with-incompatible-bdb \
     --disable-maintainer-mode \
     --disable-dependency-tracking \
     BDB_LIBS="/mingw64/lib/libdb_cxx-6.2.a" \
     LIBS="-L/mingw64/qt5-static/lib -L/mingw64/lib -lqtlibpng -lqtharfbuzz -lqtpcre2 -lz -lzstd -lfreetype -lbz2 -lpng16 -lgraphite2 -lbrotlidec -lbrotlicommon -lpcre2-8 -lusp10 -lrpcrt4 -lgdi32 -lole32 -loleaut32 -luuid -luser32 -lws2_32 -ladvapi32 -lkernel32" \
     LDFLAGS="-static -static-libgcc -static-libstdc++ -L/mingw64/qt5-static/lib -L/mingw64/qt5-static/share/qt5/plugins/platforms -L/mingw64/qt5-static/share/qt5/plugins/styles"
   ```
6. Compila los ejecutables en paralelo:
   ```bash
   make -j$(nproc)
   ```
7. Los binarios listos con el nuevo diseño incrustado se generarán en:
   * Billetera gráfica: `src/qt/korsh-qt.exe`
   * Nodo demonio: `src/korshd.exe`
   * Interfaz CLI: `src/korsh-cli.exe`

---

### B. Cómo Compilar para LINUX (Ubuntu / Debian x86_64)

1. Instala los paquetes requeridos:
   ```bash
   sudo apt-get update
   sudo apt-get install -y build-essential libtool autotools-dev automake pkg-config bsdmainutils python3 \
       libevent-dev libboost-system-dev libboost-filesystem-dev libboost-chrono-dev \
       libboost-test-dev libboost-thread-dev libssl-dev libdb5.3++-dev libdb5.3-dev \
       qtbase5-dev qttools5-dev-tools libprotobuf-dev protobuf-compiler libqrencode-dev
   ```
2. Ejecuta autogen y configure:
   ```bash
   ./autogen.sh
   ./configure --with-gui=qt5 --with-incompatible-bdb --disable-tests --disable-bench
   ```
3. Compila:
   ```bash
   make -j$(nproc)
   ```
4. El ejecutable de la billetera estará en: `src/qt/korsh-qt`.

---

### C. Cómo Compilar para macOS (Apple Silicon ARM64 / Intel)

1. Instala dependencias con Homebrew:
   ```bash
   brew install autoconf automake libtool boost berkeley-db@4 openssl libevent qt@5 qrencode
   ```
2. Exporta rutas de Homebrew:
   ```bash
   export PATH="/opt/homebrew/opt/qt@5/bin:$PATH"
   export LDFLAGS="-L/opt/homebrew/opt/qt@5/lib -L/opt/homebrew/opt/berkeley-db@4/lib"
   export CPPFLAGS="-I/opt/homebrew/opt/qt@5/include -I/opt/homebrew/opt/berkeley-db@4/include"
   ```
3. Configura y compila:
   ```bash
   ./autogen.sh
   ./configure --with-gui=qt5 --with-incompatible-bdb
   make -j$(sysctl -n hw.ncpu)
   ```
4. Crea el paquete de aplicación `.dmg`:
   ```bash
   make deploy
   ```

---

### D. Compilación Automática en GitHub (Recomendado)

Tu repositorio ya incluye el flujo automatizado en `.github/workflows/build-windows-msys2.yml`:
1. Haz commit de estos cambios:
   ```bash
   git add .
   git commit -m "feat(ui): Rediseño oficial Qt Luxor Obsidian y Verde Neon"
   git tag v0.0.2-final
   git push origin main --tags
   ```
2. GitHub Actions compilará automáticamente en sus servidores en la nube y generará los `.exe` de Windows y los paquetes de Linux y macOS como artefactos de descarga limpia.
