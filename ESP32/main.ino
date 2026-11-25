
#include <Arduino.h>

// ========================================================
// CONFIGURACION DE HARDWARE
// ========================================================

constexpr uint8_t PIN_PRESION = 34;
constexpr uint8_t PIN_VALVULA = 25;
constexpr uint8_t PIN_BOMBA   = 26;
constexpr uint8_t PIN_NIVEL   = 27;
constexpr uint8_t PIN_CIERRE  = 32;

// Sensores adicionales
constexpr bool USAR_SENSOR_NIVEL  = false;
constexpr bool USAR_SENSOR_CIERRE = false;

// Polaridad de las etapas de potencia
constexpr uint8_t RELE_ACTIVO   = HIGH;
constexpr uint8_t RELE_INACTIVO = LOW;

// ========================================================
// CONFIGURACION HIDRAULICA
// ========================================================

// Umbrales provisionales, expresados en bar.
constexpr float PRESION_BAJA = 0.5;
constexpr float PRESION_ALTA = 1.0;

// Sensor analogico de ejemplo:
// Rango: 0-6 bar
// Alimentacion: 5 V
// Salida: 0.5-4.5 V

constexpr float SENSOR_BAR_MAX = 6.0;
constexpr float SENSOR_V_MIN   = 0.5;
constexpr float SENSOR_V_MAX   = 4.5;

// Divisor resistivo:
// R1 = 10 kOhm
// R2 = 15 kOhm
constexpr float DIVISOR = 0.6;

// ========================================================
// TEMPORIZADORES (milisegundos)
// ========================================================

constexpr uint32_t TIEMPO_PERDIDA     = 5000;
constexpr uint32_t TIEMPO_RECUPERACION = 10000;
constexpr uint32_t TIEMPO_CIERRE      = 3000;
constexpr uint32_t TIMEOUT_CIERRE     = 10000;
constexpr uint32_t TIEMPO_REPOSO      = 3000;
constexpr uint32_t INTERVALO_LECTURA  = 100;
constexpr uint32_t INTERVALO_LOG      = 5000;

// ========================================================
// MAQUINA DE ESTADOS
// ========================================================

enum class Estado {
    INICIALIZACION,
    RED_PUBLICA,
    AISLAMIENTO,
    TINACO,
    RESTAURACION,
    FALLO
};

Estado estado = Estado::INICIALIZACION;

uint32_t inicioEstado = 0;
uint32_t ultimaLectura = 0;
uint32_t ultimoLog = 0;

float presion = 0.0;
bool sensorValido = false;

bool presionAlta = false;
bool presionBaja = false;

uint32_t inicioPresionAlta = 0;
uint32_t inicioPresionBaja = 0;

bool valvulaAbierta = false;
bool bombaHabilitada = false;

// ========================================================
// FUNCIONES DE CONTROL
// ========================================================

void controlarValvula(bool abrir) {
    // Interbloqueo: nunca abrir con bomba habilitada.
    if (abrir && bombaHabilitada) {
        return;
    }

    digitalWrite(
        PIN_VALVULA,
        abrir ? RELE_ACTIVO : RELE_INACTIVO
    );

    valvulaAbierta = abrir;
}

bool nivelSuficiente() {
    if (!USAR_SENSOR_NIVEL) {
        return true;
    }

    // Contacto cerrado a GND = nivel suficiente.
    return digitalRead(PIN_NIVEL) == LOW;
}

bool cierreConfirmado() {
    if (!USAR_SENSOR_CIERRE) {
        return true;
    }

    // Contacto cerrado a GND = valvula cerrada.
    return digitalRead(PIN_CIERRE) == LOW;
}

void controlarBomba(bool habilitar) {
    if (habilitar) {
        if (valvulaAbierta ||
            !nivelSuficiente() ||
            !cierreConfirmado() ||
            !sensorValido) {
            habilitar = false;
        }
    }

    digitalWrite(
        PIN_BOMBA,
        habilitar ? RELE_ACTIVO : RELE_INACTIVO
    );

    bombaHabilitada = habilitar;
}

void estadoSeguro() {
    controlarBomba(false);
    controlarValvula(false);
}

// ========================================================
// SENSOR DE PRESION
// ========================================================

bool leerPresion(float &resultado) {
    uint32_t acumulado = 0;

    for (int i = 0; i < 16; i++) {
        acumulado += analogReadMilliVolts(PIN_PRESION);
    }

    float voltajeADC = acumulado / 16000.0f;
    float voltajeSensor = voltajeADC / DIVISOR;

    // Detectar valores fuera del rango electrico esperado.
    if (voltajeSensor < 0.35f ||
        voltajeSensor > 4.65f) {
        return false;
    }

    resultado =
        (voltajeSensor - SENSOR_V_MIN) *
        SENSOR_BAR_MAX /
        (SENSOR_V_MAX - SENSOR_V_MIN);

    resultado = constrain(
        resultado,
        0.0f,
        SENSOR_BAR_MAX
    );

    return true;
}

// ========================================================
// HISTÉRESIS Y CONFIRMACION TEMPORAL
// ========================================================

void actualizarPresion(uint32_t ahora) {
    sensorValido = leerPresion(presion);

    if (!sensorValido) {
        presionAlta = false;
        presionBaja = false;
        return;
    }

    if (presion >= PRESION_ALTA) {
        if (!presionAlta) {
            inicioPresionAlta = ahora;
        }
        presionAlta = true;
    } else {
        presionAlta = false;
    }

    if (presion <= PRESION_BAJA) {
        if (!presionBaja) {
            inicioPresionBaja = ahora;
        }
        presionBaja = true;
    } else {
        presionBaja = false;
    }
}

bool redDisponible(uint32_t ahora) {
    return sensorValido &&
           presionAlta &&
           ahora - inicioPresionAlta >=
               TIEMPO_RECUPERACION;
}

bool redInterrumpida(uint32_t ahora) {
    return sensorValido &&
           presionBaja &&
           ahora - inicioPresionBaja >=
               TIEMPO_PERDIDA;
}

// ========================================================
// REGISTRO DE EVENTOS
// ========================================================

const char* nombreEstado(Estado e) {
    switch (e) {
        case Estado::INICIALIZACION:
            return "INICIALIZACION";
        case Estado::RED_PUBLICA:
            return "RED_PUBLICA";
        case Estado::AISLAMIENTO:
            return "AISLAMIENTO";
        case Estado::TINACO:
            return "TINACO";
        case Estado::RESTAURACION:
            return "RESTAURACION";
        case Estado::FALLO:
            return "FALLO";
    }

    return "DESCONOCIDO";
}

void cambiarEstado(Estado nuevo) {
    estado = nuevo;
    inicioEstado = millis();

    Serial.printf(
        "[EVENTO] Estado: %s\n",
        nombreEstado(estado)
    );
}

void registrarFallo(const char* motivo) {
    estadoSeguro();

    Serial.printf("[FALLO] %s\n", motivo);

    cambiarEstado(Estado::FALLO);
}

void imprimirEstado() {
    Serial.printf(
        "[STATUS] Estado=%s | "
        "Presion=%.2f bar | "
        "Sensor=%s | "
        "EV1=%s | P1=%s\n",
        nombreEstado(estado),
        presion,
        sensorValido ? "OK" : "ERROR",
        valvulaAbierta ? "ABIERTA" : "CERRADA",
        bombaHabilitada ? "HABILITADO" : "INHIBIDO"
    );
}

// ========================================================
// INICIALIZACION
// ========================================================

void setup() {
    digitalWrite(PIN_VALVULA, RELE_INACTIVO);
    digitalWrite(PIN_BOMBA, RELE_INACTIVO);

    pinMode(PIN_VALVULA, OUTPUT);
    pinMode(PIN_BOMBA, OUTPUT);

    estadoSeguro();

    Serial.begin(115200);

    pinMode(PIN_PRESION, INPUT);
    pinMode(PIN_NIVEL, INPUT_PULLUP);
    pinMode(PIN_CIERRE, INPUT_PULLUP);

    analogReadResolution(12);
    analogSetPinAttenuation(
        PIN_PRESION,
        ADC_11db
    );

    Serial.println(
        "Control hidraulico domestico - Rev. 3"
    );

    cambiarEstado(Estado::INICIALIZACION);
}

// ========================================================
// BUCLE PRINCIPAL
// ========================================================

void loop() {
    uint32_t ahora = millis();

    // Actualizar sensor periodicamente.
    if (ahora - ultimaLectura >= INTERVALO_LECTURA) {
        ultimaLectura = ahora;
        actualizarPresion(ahora);

        if (!sensorValido &&
            estado != Estado::FALLO) {
            registrarFallo("Sensor de presion invalido");
        }
    }

    switch (estado) {

        // ------------------------------------------------
        case Estado::INICIALIZACION:

            estadoSeguro();

            if (redDisponible(ahora)) {
                cambiarEstado(Estado::RED_PUBLICA);
            }
            else if (redInterrumpida(ahora)) {
                cambiarEstado(Estado::AISLAMIENTO);
            }

            break;

        // ------------------------------------------------
        case Estado::RED_PUBLICA:

            controlarBomba(false);
            controlarValvula(true);

            if (redInterrumpida(ahora)) {
                controlarValvula(false);
                cambiarEstado(Estado::AISLAMIENTO);
            }

            break;

        // ------------------------------------------------
        case Estado::AISLAMIENTO:

            estadoSeguro();

            if (redDisponible(ahora)) {
                cambiarEstado(Estado::RESTAURACION);
                break;
            }

            if (ahora - inicioEstado >= TIEMPO_CIERRE) {

                if (!nivelSuficiente()) {
                    registrarFallo("Nivel insuficiente");
                    break;
                }

                if (cierreConfirmado() &&
                    redInterrumpida(ahora)) {
                    cambiarEstado(Estado::TINACO);
                }
                else if (!cierreConfirmado() &&
                         ahora - inicioEstado >=
                             TIMEOUT_CIERRE) {
                    registrarFallo("Valvula sin cerrar");
                }
            }

            break;

        // ------------------------------------------------
        case Estado::TINACO:

            controlarValvula(false);

            // No esperar la recuperacion de presion
            // si existe un problema de seguridad.
            if (!cierreConfirmado()) {
                registrarFallo("Perdida de cierre EV1");
                break;
            }

            if (!nivelSuficiente()) {
                registrarFallo("Tinaco sin agua");
                break;
            }

            if (redDisponible(ahora)) {
                controlarBomba(false);
                cambiarEstado(Estado::RESTAURACION);
                break;
            }

            controlarBomba(true);

            break;

        // ------------------------------------------------
        case Estado::RESTAURACION:

            estadoSeguro();

            if (ahora - inicioEstado >= TIEMPO_REPOSO) {

                if (redDisponible(ahora)) {
                    cambiarEstado(Estado::RED_PUBLICA);
                }
                else if (redInterrumpida(ahora)) {
                    cambiarEstado(Estado::AISLAMIENTO);
                }
            }

            break;

        // ------------------------------------------------
        case Estado::FALLO:

            estadoSeguro();

            // El fallo queda enclavado hasta reiniciar
            // el controlador tras corregir su causa.

            break;
    }

    // Telemetria serial.
    if (ahora - ultimoLog >= INTERVALO_LOG) {
        ultimoLog = ahora;
        imprimirEstado();
    }
}

