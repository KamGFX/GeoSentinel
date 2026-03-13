# GeoSentinel
## Sistema de Vitrina Interactiva y Segura para Museo de Geología 🏛️💎

Este proyecto consiste en el diseño y desarrollo de una vitrina inteligente orientada a la exhibición de minerales. El sistema busca resolver dos necesidades fundamentales de las salas de exhibición: **garantizar la seguridad** del patrimonio físico mediante control de acceso y detección de anomalías, y **mejorar la experiencia educativa** del visitante mediante interacción autónoma.

Proyecto desarrollado como trabajo de grado para el programa de Ingeniería en Electrónica y Telecomunicaciones de la Universidad del Cauca.

---

## 🚀 Características Principales

El sistema se divide en tres grandes módulos operativos:

### 1. Interacción y Experiencia del Visitante
* **Detección de presencia:** Identifica cuando un usuario se acerca a la pieza exhibida.
* **Pantalla de información:** Despliega datos educativos clave sobre el mineral (origen, composición, etc.) en una pantalla integrada.
* **Iluminación sincronizada:** Resalta visualmente el mineral de forma automática y coordinada con la información en pantalla.

### 2. Seguridad y Control de Acceso Local
* **Apertura mediante RFID:** Desbloqueo del mecanismo físico exclusivo para personal autorizado mediante tarjetas o llaveros.
* **Señalización visual:** Indicadores luminosos para accesos concedidos (verde), denegados (rojo) y alertas de intrusión (ámbar continuo).
* **Alarmas acústicas:** Alertas sonoras escalonadas, desde advertencias cortas por credenciales inválidas hasta sirenas continuas por apertura forzada.

### 3. Monitoreo y Administración Web (IoT)
* **Notificaciones en tiempo real:** Envío automático de alertas al personal de seguridad a través de Telegram ante manipulaciones indebidas.
* **Panel de administración web:** Plataforma centralizada para gestionar permisos de usuarios, consultar el historial de eventos de seguridad y auditar aperturas.
* **Análisis de datos:** Generación de métricas estadísticas (gráficos de barras y líneas) sobre la afluencia de visitantes para optimizar la disposición de la sala.

---

## 🛠️ Arquitectura del Repositorio

El proyecto sigue una estructura de monorepositorio dividida por dominios tecnológicos y documentación:

```text
/
├── docs/                   # Documentación académica (Backlog, diagramas, manuales)
├── hardware/               # Código del microcontrolador y esquemáticos electrónicos
│   ├── firmware/           # Código fuente (sensores, RFID, actuadores, conexión red)
│   └── esquematicos/       # Diagramas de conexión del circuito
└── web/                    # Plataforma de administración y bases de datos
    ├── frontend/           # Interfaz gráfica de usuario y dashboards
    ├── backend/            # Lógica del servidor y API de conexión
    └── database/           # Scripts y modelos de la base de datos
