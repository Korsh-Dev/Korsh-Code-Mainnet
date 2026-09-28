# Resumen de Correcciones y Preparación para GitHub: Korsh Core v0.0.5

Este repositorio contiene la versión **Korsh Core v0.0.5**, la cual incluye todas las actualizaciones de upstream (`v0.0.3` y `v0.0.4`), el rediseño gráfico **Luxor Obsidian & Neon Green**, y la corrección técnica integral de todos los bugs y vulnerabilidades auditados.

---

## 1. Bugs y Vulnerabilidades Corregidos

### A. Reglas de Consenso y Hard Fork Seguro (v0.0.5)
* **Programación del Hard Fork v0.0.5 (`nKSHv005ForkHeight = 25000`)**:
  - Para no invalidar los más de 13,500 bloques ya minados en Mainnet, todos los cambios que modifican reglas de consenso se programaron a partir de la altura **25,000**.
  - Los bloques históricos se mantienen 100% válidos.
* **Ajuste de Dificultad cada 20 bloques (DGW) corregido**:
  - `src/pow.cpp`: Se activó la condición de reajuste cada 20 bloques para alturas `>= nKSHv005ForkHeight`, corrigiendo el código muerto previo sin romper la cadena histórica.
* **Sistema de Sporks habilitado**:
  - `src/spork.cpp`: Previo al fork v0.0.5, se mantiene el valor hardening (`4070908800ULL`) para la cadena histórica; a partir del bloque 25,000, los sporks se vuelven plenamente gobernables y operativos.

### B. Corrección de la Trampa de Evo Masternodes en Mainnet
* **Interfaz Gráfica Qt (`src/qt/korshfeatures.h` y `src/qt/masternodelist.cpp`)**:
  - La opción *"Evo (7,500 KSH)"* en el asistente se condicionó a `Params().GetDefaultPlatformP2PPort() != 0`. Dado que en Mainnet los puertos de plataforma están desactivados (valor 0), la opción queda oculta para proteger a los usuarios de perder comisiones.
* **Validador de Transacciones Especiales (`src/evo/specialtxman.cpp`)**:
  - Si se intenta registrar un Evo masternode en una red con plataforma inactiva, ahora retorna un error claro `bad-protx-platform-disabled` en lugar de fallar por colisión duplicada `0 == 0`.

### C. Redundancia de Red y Eliminación del Punto Único de Fallo (SPOF)
* **Semillas P2P Fijas y Semilla DNS (`src/chainparams.cpp`)**:
  - Se configuró la semilla DNS oficial en `vSeeds`: **`seed.korsh.org`** (resolviendo a `195.26.244.209`).
  - Se añadió un segundo nodo semilla fijo verificado en Mainnet: `185.251.19.161:9777` en formato BIP155 junto con el nodo primario `195.26.244.209:9777`. La red ya cuenta con resolución DNS dinámica y redundancia de servidores.

### D. Correcciones en Testnet y Devnet
* **BIP34Hash (`src/chainparams.cpp`)**:
  - Se vinculó `consensus.BIP34Hash = consensus.hashGenesisBlock` en Testnet y Devnet, eliminando el hash residual obsoleto de Dash.
* **Aserción Tautológica (`src/chainparams.cpp`)**:
  - Se removió `assert(hash == hash)` en Devnet y se configuró correctamente `BIP34Hash`.

### E. Compatibilidad de Verificación de Mensajes
* **Firma y Verificación (`src/util/message.cpp` y `src/util/message.h`)**:
  - `MessageVerify` ahora implementa un fallback automático hacia `MESSAGE_MAGIC_LEGACY` ("DarkCoin Signed Message:\n"), permitiendo verificar tanto firmas históricas como firmas con el nuevo magic de Korsh.

### F. Correcciones Menores de Código y Assets
* **Especificador de formato en logs (`src/masternode/payments.cpp`)**:
  - Se corrigió `%s` por `%d` en `LogPrintf` para la variable entera `nBlockHeight`.
* **Mapeo de iconos en Qt (`src/qt/korsh.qrc`)**:
  - Se corrigió el alias `checkbox_partly_checked_disabled_light` que apuntaba por error al recurso oscuro.
* **Documentación (`doc/korsh-launch.md`)**:
  - Se actualizaron los puertos documentados a P2P 9777, RPC 9776 y Tor 9775.

---

## 2. Instrucciones para Desplegar a GitHub

Abre tu terminal en la carpeta de tu Escritorio:
```bash
cd "C:\Users\lewis\Desktop\Korsh-Core-v0.0.5"
```

### Paso 1: Verificar el estado y las ramas
```bash
git status
git log -n 3 --oneline
```

### Paso 2: Configurar tu repositorio remoto (si deseas apuntar a otro repo)
Para ver los remotes actuales:
```bash
git remote -v
```
Para cambiar la URL de tu repositorio personal:
```bash
git remote set-url origin https://github.com/TU_USUARIO/TU_REPOSITORIO.git
```

### Paso 3: Subir a GitHub
Para subir la rama `main` actualizada:
```bash
git push origin main
```
O para publicar la rama de correcciones:
```bash
git push origin v0.0.5-fixes
```

Para crear el tag de la versión `v0.0.5`:
```bash
git tag v0.0.5
git push origin v0.0.5
```
