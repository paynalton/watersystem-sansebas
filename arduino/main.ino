
// ========================================================
// CONTROL HIDRAULICO DOMESTICO
// Arduino UNO R3 - ATmega328P
// Revision 3
// ========================================================

#include <Arduino.h>

// ========================================================
// CONFIGURACION DE PINES
// ========================================================

constexpr uint8_t PIN_PRESION = A0;
constexpr uint8_t PIN_VALVULA = 7;
constexpr uint8_t PIN_BOMBA   = 8;
constexpr uint8_t PIN_NIVEL   = 2;
constexpr uint8_t PIN_CIERRE  = 3;
constexpr uint8_t PIN_LED     = LED_BUILTIN;

// Sensores opcionales
constexpr bool USAR_SENSOR_NIVEL  = false;
constexpr bool USAR_SENSOR_CIERRE = false;

// Polaridad de las etapas de potencia
constexpr uint8_t RELE_ACTIVO   = HIGH;
constexpr uint8_t RELE_INACTIVO = LOW;

// ========================================================
// CONFIGURACION DEL SENSOR DE PRESION
// ========================================================

// Sensor de ejemplo: 0-6 bar, salida de 0.5-4.5 V
constexpr float SENSOR_BAR_MAX = 6.0;
constexpr float SENSOR_V_MIN   = 0.5;
constexpr float SENSOR_V_MAX   = 4.5;

// Referencia ADC nominal del Arduino UNO
constexpr float ADC_REFERENCIA = 5.0;
constexpr float ADC_MAXIMO     = 1023.0;

// Umbrales provisionales en bar
constexpr float PRESION_BAJA = 0.5;
constexpr float PRESION_ALTA = 1.0;

// ========================================================
// TEMPORIZADORES (milisegundos)
// ========================================================

constexpr uint32_t TIEMPO_PERDIDA      = 5000;
constexpr uint32_t TIEMPO_RECUPERACION = 10000;
constexpr uint32_t TIEMPO_CIERRE       = 3000;
constexpr uint32_t TIMEOUT_CIERRE      = 10000;
constexpr uint32_t TIEMPO_REPOSO       = 3000;
constexpr uint32_t INTERVALO_LECTURA   = 100;
constexpr uint32_t INTERVALO_LOG       = 5000;

// ========================================================
// MAQUINA DE ESTADOS
// ========================================================

enum class Estado : uint8_t {
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
// SENSORES AUXILIARES
// ========================================================

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

// ========================================================
// CONTROL DE ACTUADORES
// ========================================================

void controlarValvula(bool abrir) {

    // Interbloqueo de seguridad.
    if (abrir && bombaHabilitada) {
        return;
    }

    digitalWrite(
        PIN_VALVULA,
        abrir ? RELE_ACTIVO : RELE_INACTIVO
    );

    valvulaAbierta = abrir;
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
// LECTURA DE PRESION
// ========================================================

bool leerPresion(float &resultado) {

    uint32_t acumulado = 0;

    for (uint8_t i = 0; i < 16; i++) {
        acumulado += analogRead(PIN_PRESION);
    }

    float promedio = acumulado / 16.0;

    float voltaje =
        promedio * ADC_REFERENCIA / ADC_MAXIMO;

    // Detectar lecturas electricamente anormales.
    if (voltaje < 0.35 || voltaje > 4.65) {
        return false;
    }

    resultado =
        (voltaje - SENSOR_V_MIN) *
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
// HISTERESIS Y ESTABILIDAD DE PRESION
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

// F() mantiene las cadenas constantes en memoria Flash.

const __FlashStringHelper* nombreEstado(Estado e) {

    switch (e) {

        case Estado::INICIALIZACION:
            return F("INICIALIZACION");

        case Estado::RED_PUBLICA:
            return F("RED_PUBLICA");

        case Estado::AISLAMIENTO:
            return F("AISLAMIENTO");

        case Estado::TINACO:
            return F("TINACO");

        case Estado::RESTAURACION:
            return F("RESTAURACION");

        case Estado::FALLO:
            return F("FALLO");
    }

    return F("DESCONOCIDO");
}

void cambiarEstado(Estado nuevo) {

    estado = nuevo;
    inicioEstado = millis();

    Serial.print(F("[EVENTO] Estado: "));
    Serial.println(nombreEstado(estado));
}

void registrarFallo(
    const __FlashStringHelper* motivo
) {

    estadoSeguro();

    digitalWrite(PIN_LED, HIGH);

    Serial.print(F("[FALLO] "));
    Serial.println(motivo);

    cambiarEstado(Estado::FALLO);
}

void imprimirEstado() {

    Serial.print(F("[STATUS] Estado="));
    Serial.print(nombreEstado(estado));

    Serial.print(F(" | Presion="));
    Serial.print(presion, 2);
    Serial.print(F(" bar"));

    Serial.print(F(" | Sensor="));
    Serial.print(
        sensorValido ? F("OK") : F("ERROR")
    );

    Serial.print(F(" | EV1="));
    Serial.print(
        valvulaAbierta ? F("ABIERTA") : F("CERRADA")
    );

    Serial.print(F(" | P1="));
    Serial.println(
        bombaHabilitada ? F("HABILITADO") : F("INHIBIDO")
    );
}

// ========================================================
// INICIALIZACION
// ========================================================

void setup() {

    // Establecer las salidas en estado seguro.
    digitalWrite(PIN_VALVULA, RELE_INACTIVO);
    digitalWrite(PIN_BOMBA, RELE_INACTIVO);

    pinMode(PIN_VALVULA, OUTPUT);
    pinMode(PIN_BOMBA, OUTPUT);
    pinMode(PIN_LED, OUTPUT);

    pinMode(PIN_PRESION, INPUT);
    pinMode(PIN_NIVEL, INPUT_PULLUP);
    pinMode(PIN_CIERRE, INPUT_PULLUP);

    analogReference(DEFAULT);

    digitalWrite(PIN_LED, LOW);

    estadoSeguro();

    Serial.begin(115200);

    Serial.println(
        F("Control hidraulico domestico - Rev. 3")
    );

    Serial.println(F("Arduino UNO R3"));

    cambiarEstado(Estado::INICIALIZACION);
}

// ========================================================
// BUCLE PRINCIPAL
// ========================================================

void loop() {

    uint32_t ahora = millis();

    // Actualizar periodicamente el sensor de presion.
    if (ahora - ultimaLectura >= INTERVALO_LECTURA) {

        ultimaLectura = ahora;

        actualizarPresion(ahora);

        if (!sensorValido &&
            estado != Estado::FALLO) {

            registrarFallo(
                F("Sensor de presion invalido")
            );
        }
    }

    switch (estado) {

        // ------------------------------------------------
        case Estado::INICIALIZACION:

            estadoSeguro();

            if (redDisponible(ahora)) {

                cambiarEstado(Estado::RED_PUBLICA);

            } else if (redInterrumpida(ahora)) {

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

                    registrarFallo(
                        F("Nivel insuficiente")
                    );

                    break;
                }

                if (cierreConfirmado() &&
                    redInterrumpida(ahora)) {

                    cambiarEstado(Estado::TINACO);

                } else if (
                    !cierreConfirmado() &&
                    ahora - inicioEstado >= TIMEOUT_CIERRE
                ) {

                    registrarFallo(
                        F("Valvula sin cerrar")
                    );
                }
            }

            break;

        // ------------------------------------------------
        case Estado::TINACO:

            controlarValvula(false);

            if (!cierreConfirmado()) {

                registrarFallo(
                    F("Perdida de cierre EV1")
                );

                break;
            }

            if (!nivelSuficiente()) {

                registrarFallo(
                    F("Tinaco sin agua")
                );

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

                } else if (redInterrumpida(ahora)) {

                    cambiarEstado(Estado::AISLAMIENTO);
                }
            }

            break;

        // ------------------------------------------------
        case Estado::FALLO:

            estadoSeguro();

            // Fallo enclavado hasta reinicio manual.

            break;
    }

    // Registro periodico mediante puerto serial.
    if (ahora - ultimoLog >= INTERVALO_LOG) {

        ultimoLog = ahora;
        imprimirEstado();
    }
}

