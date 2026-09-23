# Captura Pantalla: plugin FFGL para Resolume Arena 7 (Windows y Mac)

Este plugin es una **fuente** (source) de Resolume. Muestra en vivo una **pantalla completa** o **una ventana**, por ejemplo PowerPoint, Chrome o Keynote. No necesita OBS, NDI, Spout ni Syphon.

En Windows usa **Windows Graphics Capture**, la misma API que usa la herramienta Recortes. En Mac usa **ScreenCaptureKit**, la API de captura de Apple. Las dos capturan bien las ventanas con aceleración por GPU, como los navegadores y los juegos.

## Requisitos
- **Windows:** Windows 10 versión 1903 o superior. Se recomienda Windows 11: ahí se oculta el borde amarillo de captura.
- **Mac:** macOS 12.3 o superior. Un solo plugin sirve para chips Apple (M1 a M4) e Intel.
- Resolume Arena o Avenue **7.x** (hecho para 7.18.2). Para **Arena 6 en Mac** hay una versión aparte (ver abajo).
- Para compilar: **Visual Studio 2022** (la edición Community es gratis) con la carga de trabajo **"Desarrollo para el escritorio con C++"**. Esa carga de trabajo ya incluye CMake y el Windows SDK.

## Instalar en Mac
1. Descarga **`ScreenCapture-mac.zip`** de la pestaña **Releases** y ábrelo con doble clic. Aparece **`ScreenCapture.bundle`**.
2. Cierra Arena y copia `ScreenCapture.bundle` en `Documentos/Resolume Arena/Extra Effects`.
3. Abre Arena. La primera vez, macOS pide el permiso de **Grabación de pantalla** para Arena. Actívalo en *Ajustes del Sistema → Privacidad y seguridad → Grabación de pantalla y audio del sistema* y reinicia Arena.
4. Si macOS dice que no puede verificar el desarrollador, abre Terminal y ejecuta:
   `xattr -dr com.apple.quarantine ~/Documents/"Resolume Arena"/"Extra Effects"/ScreenCapture.bundle`

En Mac, *Restaurar si se minimiza* no aparece. macOS tampoco dibuja las ventanas minimizadas, así que Arena se queda con la última imagen.

## Versión para Arena 6 (solo Mac)
Arena 6 usa un sistema de plugins más antiguo (FFGL 1.6), así que tiene su propio archivo: **`ScreenCapture-mac-arena6.zip`**, que trae `ScreenCaptureArena6.bundle`. Se copia en `Documentos/Resolume Arena 6/Extra Effects`.

Arena 6 no tiene menús desplegables, así que la fuente se elige **escribiendo** en el campo **Fuente**:
- `pantalla 1`, `pantalla 2`… para una pantalla completa.
- Parte del nombre de un programa o ventana: `chrome`, `keynote`, `powerpoint`, `safari`.

Si no encuentra lo que escribiste, anota en `Documentos/CapturaPantalla-log.txt` la lista de fuentes disponibles. Los demás controles son *Proporcion* (mantener la forma de la imagen), *Mostrar cursor* y los cuatro *Recorte*.

## Compilar e instalar en Windows
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
| **Restaurar si se minimiza** | Si la ventana capturada se minimiza, el plugin la vuelve a abrir **detrás de todas las demás**, sin quitarte el foco, para que siga viéndose en vivo en Arena. Desactivado: se congela la última imagen hasta que la restaures. |
| **Solo contenido** (Windows) | Captura solo la **foto o el video** de la ventana, sin barras de herramientas ni bordes. Sirve para la app **Fotos**, el **Reproductor multimedia** y **Películas y TV**. El recorte se ajusta solo si cambias el tamaño de la ventana o la foto. Los controles que aparecen *encima* del video (Play, barra de tiempo) se esconden solos si no mueves el mouse sobre el reproductor. |
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
El workflow `release.yml` compila el plugin para Windows y Mac con esa versión y crea un Release con `ScreenCapture.dll`, `ScreenCapture-mac.zip` y `latest.json` (versión, URLs, SHA-256 y notas). Los plugins instalados leen siempre `releases/latest/download/latest.json`.

**Importante:** el primer `.dll` que instales a mano tiene que salir de un Release (de la pestaña Releases, no de `build.bat`). Las compilaciones locales no tienen la dirección de actualización y no buscan versiones nuevas. Para probar localmente con actualizaciones:
```bash
cmake -S . -B build -A x64 -DPLUGIN_VERSION=1.0.0 -DUPDATE_MANIFEST_URL=https://github.com/USUARIO/REPO/releases/latest/download/latest.json
```

**Seguridad:** solo se aceptan descargas por HTTPS y el archivo debe coincidir con el SHA-256 del manifiesto. Aun así, quien controle tu cuenta de GitHub puede publicar un plugin. Protege la cuenta con autenticación de dos factores.

## Notas y limitaciones
- **Windows no dibuja las ventanas minimizadas.** Por eso existe *Restaurar si se minimiza*, que la devuelve detrás de las demás. Con esa opción apagada, Arena se queda con la última imagen.
- En la lista, la ventana se guarda por posición. Al reabrir una composición puede apuntar a otra ventana, por eso conviene usar **Buscar ventana**.
- En Windows 10 aparece un **borde amarillo** alrededor de lo que se captura. Windows 11 permite ocultarlo y el plugin lo hace automáticamente.
- La captura corre en un hilo propio, así que Arena nunca espera a Windows al cambiar de fuente.
- Cada cuadro se copia de la GPU a la CPU y de vuelta a OpenGL. En 1080p va fluido. Para 4K a 60 fps se puede optimizar más adelante con `WGL_NV_DX_interop`, que evita la copia.
- Los errores aparecen en el log de Resolume con el prefijo `[Captura Pantalla]`. Además, cada paso queda anotado en `Documentos\CapturaPantalla-log.txt`; si algo falla, envía ese archivo.

## Zócalo (lower third animado)
Segundo plugin del proyecto: **Zocalo**, en *Sources*. Presenta a una persona con **foto, nombre y subtítulo**, con una entrada animada: la foto aparece, la barra se despliega y los textos se deslizan. Tiene fondo transparente para ponerlo sobre la cámara o un video en otra capa.

| Parámetro | Qué hace |
|---|---|
| **Foto** | Imagen de la persona (jpg, png, heic...). Se recorta sola al círculo. Sin foto muestra las **iniciales**. |
| **Nombre / Subtitulo** | Los textos. Si los cambias con el zócalo visible, la barra se ajusta suavemente. |
| **Posicion** | Abajo izquierda, abajo derecha, abajo centro, arriba izquierda, arriba derecha. Si lo cambias con el zócalo visible, **viaja animado** a la nueva esquina. |
| **Entrar / Salir** | Botones para dispararlo en vivo. |
| **Entrar al activar** | Entra solo cada vez que activas el clip. |
| **Salir despues** | Sale solo después de N segundos (0 = nunca). |
| **Tamano / Velocidad** | Escala del conjunto y rapidez de la animación. |
| **Foto redonda** | Círculo o cuadrado con esquinas redondeadas. |
| **Color barra / Color acento** | Fondo de la barra (con transparencia) y color del anillo, la línea y las iniciales. El texto se pone blanco u oscuro según la barra. |
| **Salir con otro clip** | Al disparar **otro clip** en Resolume, el zócalo **sale con su animación**. Si vuelves a disparar el mismo zócalo, repite la entrada. Necesita la salida OSC de Arena (ver abajo). |
| **Puerto OSC** | Puerto por el que escucha a Arena (7001, el que Arena usa por defecto). |

### Activar "Salir con otro clip" (una sola vez)
Resolume no avisa a los plugins cuando disparas otro clip, pero sí puede avisar por **OSC** dentro del mismo computador:
1. En Arena: **Arena → Preferences → OSC**.
2. Activa **OSC Output**. Dirección **127.0.0.1**, puerto **7001**. Si la opción existe en tu versión, marca **Output all OSC messages**.
3. Pon el zócalo **en su propia capa**. Al disparar un clip en otra capa, el zócalo sale animado. Si cambias de clip *en la misma capa*, Resolume corta el zócalo al instante, salvo que esa capa tenga un tiempo de **Transition**; con 1 segundo o más, la salida alcanza a verse.

Si no sale, revisa `Documentos/Zocalo-log.txt`: debe decir "OSC: llegan mensajes de Resolume" y anotar los mensajes de clips que recibe.

Idea de uso: una fila de clips con un zócalo por orador, cada uno con su foto y nombre, y los disparas con un clic o un Stream Deck.

Archivos: `Zocalo.dll` (Windows) y `Zocalo-mac.zip` (Mac), en la misma página de Releases. Se instalan igual que Captura Pantalla y se actualizan solos. Su registro está en `Documentos/Zocalo-log.txt`.

## Estructura
```
src/ScreenCapture.*    Plugin FFGL: parámetros, shader y dibujo (común)
src/CaptureEngine.h    Interfaz de captura
src/CaptureTargets.*   Lista de pantallas y ventanas (búsqueda común)
src/Updater.*          Lógica de actualizaciones remotas (común)
src/Platform.h         Lo que cada sistema aporta: descargas, ajustes, instalar
src/win/               Windows: Windows Graphics Capture, WinHTTP, ventana Win32
src/mac/               Mac: ScreenCaptureKit, NSURLSession, ventana AppKit
src/arena6/            Plugin para Arena 6 (FFGL 1.6), reutiliza la captura de Mac
src/zocalo/            Plugin Zócalo: animación, diseño (shader) e icono
src/Drawing.h          Texto y fotos: GDI + WIC (Windows), CoreText + ImageIO (Mac)
.github/workflows/     build.yml (compilación de prueba) y release.yml (publica Windows + Mac)
third_party/ffgl       SDK FFGL oficial de Resolume (github.com/resolume/ffgl)
third_party/glew-2.1.0 GLEW, que usa el SDK en Windows
third_party/ffgl-1.6   SDK FFGL 1.6 de Resolume (2018), para Arena 6
```
