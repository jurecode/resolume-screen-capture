# Captura Pantalla: plugin FFGL para Resolume Arena 7 (Windows)

Este plugin es una **fuente** (source) de Resolume. Muestra en vivo una **pantalla completa** o **una ventana** de Windows, por ejemplo PowerPoint, Chrome o Keynote Viewer. No necesita OBS, NDI ni Spout.

Usa **Windows Graphics Capture**, la misma API que usa la herramienta Recortes de Windows, así que captura bien las ventanas con aceleración por GPU, como los navegadores y los juegos.

## Requisitos
- Windows 10 versión 1903 o superior. Se recomienda Windows 11: ahí se oculta el borde amarillo de captura.
- Resolume Arena o Avenue 7.x de 64 bits (probado para 7.18.2).
- Para compilar: **Visual Studio 2022** (la edición Community es gratis) con la carga de trabajo **"Desarrollo para el escritorio con C++"**. Esa carga de trabajo ya incluye CMake y el Windows SDK.

## Compilar e instalar
1. Copia la carpeta `ScreenCapture` al PC con Windows.
2. Haz doble clic en `build.bat`. Si dice que no encuentra `cmake`, ábrelo desde **"Developer Command Prompt for VS 2022"**.
3. El script compila `build\Release\ScreenCapture.dll` y lo copia a `Documentos\Resolume Arena\Extra Effects`.
4. Reinicia Arena. El plugin aparece en **Sources** como **Captura Pantalla**.

Si usas otra carpeta de plugins, revísala en *Arena → Preferences → Video → Plugins* y copia el `.dll` ahí.

### Compilar sin tener Windows (GitHub Actions)
Sube este proyecto a un repositorio de GitHub. El workflow `.github/workflows/build.yml` compila el plugin en la nube y deja `ScreenCapture.dll` para descargar en la pestaña **Actions**, en el apartado *Artifacts*.

## Parámetros
| Parámetro | Qué hace |
|---|---|
| **Fuente** | Lista de `(ninguna)`, las pantallas (`Pantalla N`) y las ventanas abiertas (`Ventana: título [programa.exe]`). |
| **Actualizar lista** | Vuelve a leer las ventanas abiertas. Úsalo después de abrir un programa nuevo. |
| **Buscar ventana** | Escribe parte del título o del programa, por ejemplo `powerpnt`, `chrome` o `pantalla 2`. Es la forma **recomendada** para shows: se guarda con la composición y, si la ventana se cierra y se vuelve a abrir, la captura vuelve sola en unos 2 segundos. |
| **Ajuste** | *Estirar* llena todo aunque deforme la imagen. *Encajar* mantiene las proporciones con bordes transparentes. *Rellenar* mantiene las proporciones y recorta lo que sobra. |
| **Mostrar cursor** | Muestra u oculta el puntero del mouse en la captura. |
| **Recorte** (izquierda, derecha, arriba, abajo) | Recorta los bordes, por ejemplo la barra de título o la del navegador. Cada control llega hasta el 50 %. |
| **Actualización** | Estado del plugin y botones *Instalar*, *Más tarde* y *Omitir versión*. Solo aparece en las versiones publicadas (ver abajo). |

## Actualizaciones remotas
Las versiones publicadas con GitHub Releases revisan si hay actualizaciones al abrir Arena (a los 20 s) y luego cada 6 horas.

**Cuando hay una versión nueva:**
1. Windows muestra una notificación en la esquina. No roba el foco ni aparece sobre la salida del proyector, y Windows la retiene mientras haya una app en pantalla completa.
2. En el grupo **Actualización** de cualquier clip de Captura Pantalla aparecen tres botones:
   - **Instalar vX.Y.Z**: descarga el plugin, verifica su SHA-256 y reemplaza el `.dll`.
   - **Más tarde**: no vuelve a avisar durante 24 horas.
   - **Omitir versión**: no vuelve a avisar de esa versión.
3. Después de instalar, el botón dice *"reinicia Arena"*. Windows no permite cambiar un plugin que está en uso, así que la versión nueva se carga la próxima vez que abras Arena. **El show en curso no se interrumpe.**

Nada se instala solo: siempre pulsas tú el botón. Si una actualización falla, la versión que ya tenías sigue funcionando.

### Cómo publicar una actualización
Una sola vez: sube este proyecto a un repositorio **público** de GitHub. Las descargas de un repositorio privado piden credenciales.

Cada vez que cambies algo:
```bash
git commit -am "Descripción del cambio"
git tag -a v1.1.0 -m "Texto que verán los usuarios en el aviso"
git push origin main v1.1.0
```
El workflow `release.yml` compila el plugin con esa versión y crea un Release con `ScreenCapture.dll` y `latest.json` (versión, URL, SHA-256 y notas). Los plugins instalados leen siempre `releases/latest/download/latest.json`.

**Importante:** el primer `.dll` que instales a mano tiene que salir de un Release (de la pestaña Releases, no de `build.bat`). Las compilaciones locales no tienen la dirección de actualización y no buscan versiones nuevas. Para probar localmente con actualizaciones:
```bash
cmake -S . -B build -A x64 -DPLUGIN_VERSION=1.0.0 -DUPDATE_MANIFEST_URL=https://github.com/USUARIO/REPO/releases/latest/download/latest.json
```

**Seguridad:** solo se aceptan descargas por HTTPS y el archivo debe coincidir con el SHA-256 del manifiesto. Aun así, quien controle tu cuenta de GitHub puede publicar un plugin. Protege la cuenta con autenticación de dos factores.

## Notas y limitaciones
- **Las ventanas minimizadas no se capturan.** Windows no las dibuja, así que la imagen se queda en negro o transparente. Déjalas abiertas, aunque estén detrás de otras ventanas.
- En la lista, la ventana se guarda por posición. Al reabrir una composición puede apuntar a otra ventana, por eso conviene usar **Buscar ventana**.
- En Windows 10 aparece un **borde amarillo** alrededor de lo que se captura. Windows 11 permite ocultarlo y el plugin lo hace automáticamente.
- La captura corre en un hilo propio, así que Arena nunca espera a Windows al cambiar de fuente.
- Cada cuadro se copia de la GPU a la CPU y de vuelta a OpenGL. En 1080p va fluido. Para 4K a 60 fps se puede optimizar más adelante con `WGL_NV_DX_interop`, que evita la copia.
- Los errores aparecen en el log de Resolume con el prefijo `[Captura Pantalla]`. Además, cada paso queda anotado en `Documentos\CapturaPantalla-log.txt`; si algo falla, envía ese archivo.

## Estructura
```
src/ScreenCapture.*    Plugin FFGL: parámetros, shader y dibujo
src/WgcCapture.*       Captura con Windows Graphics Capture + Direct3D 11
src/CaptureTargets.*   Lista de pantallas y ventanas
src/Updater.*          Actualizaciones remotas (WinHTTP + SHA-256 + notificación)
.github/workflows/     build.yml (compilación de prueba) y release.yml (publicar actualización)
third_party/ffgl       SDK FFGL oficial de Resolume (github.com/resolume/ffgl)
third_party/glew-2.1.0 GLEW, que usa el SDK en Windows
```
