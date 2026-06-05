#include <Wire.h>
#define XPOWERS_CHIP_AXP2101 
#include <XPowersLib.h> 

XPowersAXP2101 PMU;

// --- CONFIGURATION PINS (S3) ---
#define PMU_SDA 15
#define PMU_SCL 7
#define MODEM_RX 4
#define MODEM_TX 5
#define MODEM_PWRKEY 38  
#define BOOT_BUTTON 0

// --- CONFIGURATION UTILISATEUR ---
const String NUMERO_TEL = "\"+33xxxxxxxxx\""; 

// --- FONCTIONS DE COMMUNICATION ---
String sendAT(String command, int timeout_ms) {
  String response = "";
  Serial1.println(command);
  long int time = millis();
  while ((time + timeout_ms) > millis()) {
    while (Serial1.available()) {
      response += (char)Serial1.read();
    }
  }
  return response;
}

// Fonction de validation stricte
bool validateAT(String command, String expected_response, int timeout_ms) {
  String response = sendAT(command, timeout_ms);
  return (response.indexOf(expected_response) != -1);
}

// --- SÉQUENCES D'INITIALISATION ---
void setupPMU() {
  Wire.begin(PMU_SDA, PMU_SCL);
  if (!PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, PMU_SDA, PMU_SCL)) {
    Serial.println("[ ÉCHEC FATAL ] PMU AXP2101 introuvable. Arrêt.");
    while (1) delay(1000); 
  }
  
  PMU.setDC3Voltage(3000);   
  PMU.enableDC3();
  PMU.setALDO2Voltage(3300); 
  PMU.enableALDO2();
  PMU.setBLDO2Voltage(3300);
  PMU.enableBLDO2();
  
  Serial.println("[ SUCCÈS ] Tensions PMU activées.");
}

void setupModem() {
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  pinMode(MODEM_PWRKEY, OUTPUT);
  
  // Allumage matériel silencieux
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000); 
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(10000); // Temps de boot matériel de la puce

  // 1. Vérification de la communication série
  bool modemPret = false;
  for(int i = 0; i < 5; i++) {
    if (validateAT("AT", "OK", 1000)) {
      modemPret = true;
      break;
    }
    delay(1000);
  }

  if (!modemPret) {
    Serial.println("[ ÉCHEC FATAL ] Le modem ne répond pas aux commandes AT. Arrêt.");
    while(1) delay(1000); 
  }
  Serial.println("[ SUCCÈS ] Communication AT établie avec le modem.");

  // 2. Vérification de la carte SIM
  if (validateAT("AT+CPIN?", "READY", 2000)) {
    Serial.println("[ SUCCÈS ] Carte SIM détectée et déverrouillée.");
  } else {
    Serial.println("[ ÉCHEC ] Carte SIM absente ou bloquée par un code PIN.");
  }

  // 3. Allumage du GPS
  if (validateAT("AT+CGNSPWR=1", "OK", 1000)) {
    Serial.println("[ SUCCÈS ] Puce GPS activée.");
  } else {
    Serial.println("[ ÉCHEC ] Impossible d'activer la puce GPS.");
  }

  // Configuration réseau (sans affichage car ce sont des envois de paramètres)
  sendAT("AT+CGDCONT=1,\"IP\",\"free\"", 1000);
  sendAT("AT+CMGF=1", 1000);
}

void verifierReseau() {
  bool connecte = false;
  int tentatives = 0;
  
  // On teste jusqu'à 30 fois (soit environ 1 minute)
  while (!connecte && tentatives < 30) {
    String rep = sendAT("AT+CEREG?", 2000); // CEREG est spécifique au LTE (EPS)
    
    // ,1 = Réseau local, ,5 = Roaming (valide aussi)
    if (rep.indexOf(",1") != -1 || rep.indexOf(",5") != -1) {
      Serial.println("[ SUCCÈS ] Modem connecté au réseau LTE.");
      connecte = true;
    } else {
      tentatives++;
      delay(2000);
    }
  }

  if (!connecte) {
    Serial.println("[ ÉCHEC ] Délai dépassé : impossible de s'accrocher au réseau LTE.");
  }
}

// --- FONCTIONS OPÉRATIONNELLES ---
String obtenirGPS() {
  String brute = sendAT("AT+CGNSINF", 1500);
  
  // L'indicateur "1,1" signifie que le module GPS est allumé ET a un FIX valide
  if (brute.indexOf("+CGNSINF: 1,1") != -1) {
    return brute; 
  } 
  return "NO_FIX";
}

void envoyerSMS(String position) {
  if (validateAT("AT+CMGS=\"" + NUMERO_TEL + "\"", ">", 2000)) {
    String texte = "ALERTE PHOBOS PULSAR !\n" + position;
    Serial1.print(texte);
    delay(500);
    Serial1.write(26); // Caractère de fin de message (CTRL+Z)
    
    // On lit la réponse pour confirmer l'envoi réel
    String rep = sendAT("", 5000);
    if (rep.indexOf("+CMGS:") != -1 || rep.indexOf("OK") != -1) {
       Serial.println("[ SUCCÈS ] SMS envoyé au réseau.");
    } else {
       Serial.println("[ ÉCHEC ] Erreur lors de la transmission du SMS.");
    }
  } else {
    Serial.println("[ ÉCHEC ] Le modem a refusé l'ouverture du prompt SMS.");
  }
}

// --- CORE ---
void setup() {
  Serial.begin(115200);
  delay(3000); 
  
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  
  setupPMU();
  setupModem(); 
  verifierReseau(); 
  
  Serial.println("[ ÉTAT ] Séquence de démarrage terminée. En attente d'input matériel (BOUTON BOOT).");
}

void loop() {
  // Purge silencieuse du buffer série pour éviter les fuites mémoire ou la pollution de l'UART
  while (Serial1.available()) {
    Serial1.read();
  }

  // Gestion du bouton BOOT (Appui maintenu 3 secondes)
  if (digitalRead(BOOT_BUTTON) == LOW) {
    delay(50); 
    unsigned long startTime = millis();
    
    while(digitalRead(BOOT_BUTTON) == LOW) {
      if (millis() - startTime > 3000) {
        
        String coords = obtenirGPS();
        
        if (coords == "NO_FIX") {
          Serial.println("[ ÉCHEC ] Coordonnées GPS introuvables (Pas de FIX).");
        } else {
          Serial.println("[ SUCCÈS ] FIX GPS obtenu :");
          Serial.println(coords);
          // envoyerSMS(coords); // Décommenter pour tester l'envoi
        }
        
        // Bloque ici tant que le bouton n'est pas relâché pour éviter les boucles
        while(digitalRead(BOOT_BUTTON) == LOW); 
      }
    }
  }
}