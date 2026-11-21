# BC250Display — driver básico de Windows para la AMD BC-250

Driver **WDDM display-only** (solo pantalla, sin aceleración 3D) para la placa AMD BC-250
(APU *Cyan Skillfish*, `PCI\VEN_1002&DEV_13FE`, y la variante `DEV_143F`).

Está basado en la muestra oficial **KMDOD** de Microsoft
([Windows-driver-samples/video/KMDOD](https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD), licencia MS-PL).
Esa muestra es un driver WDDM display-only completo y probado. Por eso no se empieza desde cero.

## Qué hace y qué no hace

| Funciona | No funciona (todavía) |
|---|---|
| Se instala sobre la GPU BC-250 con su nombre propio | Aceleración 3D, DirectX y Vulkan |
| Pantalla por el framebuffer UEFI/GOP (la resolución que deja el firmware) | Cambio de resolución por hardware o varios monitores |
| Blit por CPU, cursor por software y rotación | Control de relojes y ventiladores (SMU) |
| Rechaza arrancar en cualquier GPU que no sea una BC-250 (`CheckHardware`) | **Suspensión S3 y S4**: produce bugcheck por diseño, así que el instalador la desactiva |

**Por qué no hay 3D.** En Linux el soporte existe gracias a `amdgpu` y Mesa RADV. En Windows haría falta un
driver WDDM completo con gestión de memoria de GPU, colas de comandos, firmware (PSP/SMU) y un
driver de modo usuario D3D/Vulkan. AMD no publica código de su driver de Windows. Esta base es el
primer paso estable. El siguiente sería leer los registros de la GPU (BAR MMIO) desde este driver.

## Cambios respecto a KMDOD

- `src/bdd.hxx`: IDs `0x1002:0x13FE` y `0x1002:0x143F`.
- `src/bdd.cxx`: `CheckHardware()` activado en modo PCI. El driver solo arranca en una BC-250.
  También se rellenó la información del adaptador en el registro.
- `package/bc250display.inf`: se enlaza solo a la BC-250 por Hardware ID. Ya no usa la clase genérica VGA.
- Proyecto solo x64. El WDK y el SDK se descargan por NuGet.

## Compilar

Requisitos: Visual Studio 2022 con **"Desarrollo para el escritorio con C++"** y
**"MSVC v143 – bibliotecas con mitigación de Spectre (x64/x86)"**. No hace falta instalar el WDK.

```powershell
.\scripts\build.ps1 -Configuration Release
```

El resultado queda en `out\Release\` (`BC250Display.sys`, `.inf`, `.cat` y `.cer`).

> Si la ruta de la carpeta es muy larga, NuGet falla con "path too long". En ese caso clona el repo en
> una ruta corta, por ejemplo `C:\src\bc250`.

También puedes compilarlo en la nube: el workflow `.github/workflows/build.yml` lo compila en GitHub
Actions y publica el paquete como artifact.

## Instalar en la BC-250

1. En la BIOS de la BC-250, **desactiva Secure Boot**. Es obligatorio para los drivers con firma de prueba.
2. Copia `out\Release\` y `scripts\` a la BC-250.
3. En PowerShell como Administrador:

```powershell
.\scripts\install.ps1 -EnableTestSigning
```

Reinicia y luego ejecuta:

```powershell
.\scripts\install.ps1
```

4. En el Administrador de dispositivos debería aparecer **"AMD BC-250 Display-Only Adapter"**.

Para volver al adaptador básico de Microsoft:

```powershell
.\scripts\uninstall.ps1
```

## Depuración

- Visor de eventos → Sistema, origen `BC250Display`.
- Si queda en **código 43**: no había framebuffer POST/GOP. Comprueba que la BIOS arranque en modo UEFI
  con salida de vídeo de la BC-250.
- Si hay pantalla negra: arranca en modo seguro y ejecuta `uninstall.ps1`.

## Hoja de ruta

1. ✅ Display-only estable (este repo).
2. Mapear el BAR de registros (MMIO) y leer temperatura y relojes, solo lectura.
3. Cambio de modos usando el DCN (motor de display) de la GPU. Referencia: `drivers/gpu/drm/amd/display` en Linux.
4. Cualquier cosa de 3D requiere un proyecto de otra escala.

## Licencia

Código derivado de Microsoft Windows-driver-samples bajo MS-PL. Ver `LICENSE-MS-PL.txt`.
