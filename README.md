# BC250Display â€” driver bÃ¡sico de Windows para la AMD BC-250

Driver **WDDM display-only** (solo pantalla, sin aceleraciÃ³n 3D) para la placa AMD BC-250
(APU *Cyan Skillfish*, `PCI\VEN_1002&DEV_13FE`, y la variante `DEV_143F`).

EstÃ¡ basado en la muestra oficial **KMDOD** de Microsoft
([Windows-driver-samples/video/KMDOD](https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD), licencia MS-PL).
Esa muestra es un driver WDDM display-only completo y probado. Por eso no se empieza desde cero.

## QuÃ© hace y quÃ© no hace

| Funciona | No funciona (todavÃ­a) |
|---|---|
| Se instala sobre la GPU BC-250 con su nombre propio | AceleraciÃ³n 3D, DirectX y Vulkan |
| Pantalla por el framebuffer UEFI/GOP (la resoluciÃ³n que deja el firmware) | Cambio de resoluciÃ³n por hardware o varios monitores |
| Blit por CPU, cursor por software y rotaciÃ³n | Control de relojes y ventiladores (SMU) |
| Rechaza arrancar en cualquier GPU que no sea una BC-250 (`CheckHardware`) | **SuspensiÃ³n S3 y S4**: produce bugcheck por diseÃ±o, asÃ­ que el instalador la desactiva |

**Por quÃ© no hay 3D.** En Linux el soporte existe gracias a `amdgpu` y Mesa RADV. En Windows harÃ­a falta un
driver WDDM completo con gestiÃ³n de memoria de GPU, colas de comandos, firmware (PSP/SMU) y un
driver de modo usuario D3D/Vulkan. AMD no publica cÃ³digo de su driver de Windows. Esta base es el
primer paso estable. El siguiente serÃ­a leer los registros de la GPU (BAR MMIO) desde este driver.

## Cambios respecto a KMDOD

- `src/bdd.hxx`: IDs `0x1002:0x13FE` y `0x1002:0x143F`.
- `src/bdd.cxx`: `CheckHardware()` activado en modo PCI. El driver solo arranca en una BC-250.
  TambiÃ©n se rellenÃ³ la informaciÃ³n del adaptador en el registro.
- `package/bc250display.inf`: se enlaza solo a la BC-250 por Hardware ID. Ya no usa la clase genÃ©rica VGA.
- Proyecto solo x64. El WDK y el SDK se descargan por NuGet.

## Compilar

Requisitos: Visual Studio 2022 con **"Desarrollo para el escritorio con C++"** y
**"MSVC v143 â€“ bibliotecas con mitigaciÃ³n de Spectre (x64/x86)"**. No hace falta instalar el WDK.

```powershell
.\scripts\build.ps1 -Configuration Release
```

El resultado queda en `out\Release\` (`BC250Display.sys`, `.inf`, `.cat` firmado y `.cer`). El script
crea un certificado de prueba temporal, firma el `.sys` y el `.cat`, exporta el `.cer` y borra la clave privada.

### CI (GitHub Actions)

Solo se ejecuta cuando cambia algo del driver (no por cambios en el README). Compila solo Release, guarda el WDK en
caché y cancela ejecuciones repetidas. Para compilar Debug, lánzalo a mano desde la pestaña **Actions** → *Run workflow*.
El paquete se descarga desde la ejecución (artifact `BC250Display-Release`, se guarda 14 días).

> Si la ruta de la carpeta es muy larga, NuGet falla con "path too long". En ese caso clona el repo en
> una ruta corta, por ejemplo `C:\src\bc250`.

TambiÃ©n puedes compilarlo en la nube: el workflow `.github/workflows/build.yml` lo compila en GitHub
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

4. En el Administrador de dispositivos deberÃ­a aparecer **"AMD BC-250 Display-Only Adapter"**.

Para volver al adaptador bÃ¡sico de Microsoft:

```powershell
.\scripts\uninstall.ps1
```

## DepuraciÃ³n

- Visor de eventos â†’ Sistema, origen `BC250Display`.
- Si queda en **cÃ³digo 43**: no habÃ­a framebuffer POST/GOP. Comprueba que la BIOS arranque en modo UEFI
  con salida de vÃ­deo de la BC-250.
- Si hay pantalla negra: arranca en modo seguro y ejecuta `uninstall.ps1`.

## Hoja de ruta

1. âœ… Display-only estable (este repo).
2. Mapear el BAR de registros (MMIO) y leer temperatura y relojes, solo lectura.
3. Cambio de modos usando el DCN (motor de display) de la GPU. Referencia: `drivers/gpu/drm/amd/display` en Linux.
4. Cualquier cosa de 3D requiere un proyecto de otra escala.

## Licencia

CÃ³digo derivado de Microsoft Windows-driver-samples bajo MS-PL. Ver `LICENSE-MS-PL.txt`.
