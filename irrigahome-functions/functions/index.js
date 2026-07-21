const { onRequest } = require("firebase-functions/v2/https");
const { onDocumentCreated, onDocumentUpdated, onDocumentDeleted, onDocumentWritten } = require("firebase-functions/v2/firestore");
const admin = require("firebase-admin");

admin.initializeApp();

const db = admin.firestore();

const MQTT_BROKER_URL = process.env.MQTT_BROKER_URL;
const MQTT_TOPIC_COMMANDS = process.env.MQTT_TOPIC_COMMANDS || "irrigahome/commands";
const HISTORY_COLLECTION_NAME = "events";
const SCHEDULE_COLLECTIONS = ["schedule_01", "schedule_02", "schedule_03", "schedule_04"];
const MQTT_CONNECT_TIMEOUT_MS = 10_000;
const MQTT_PUBLISH_TIMEOUT_MS = 10_000;
const MQTT_RETAIN_MESSAGES = process.env.MQTT_RETAIN_MESSAGES === "true";

function toIsoOrNull(value) {
  if (!value) {
    return null;
  }

  if (typeof value.toDate === "function") {
    return value.toDate().toISOString();
  }

  if (value instanceof Date) {
    return value.toISOString();
  }

  if (typeof value === "string") {
    const parsed = new Date(value);

    if (!Number.isNaN(parsed.getTime())) {
      return parsed.toISOString();
    }
  }

  return null;
}

function toFirestoreTimestamp(value) {
  if (!value) {
    return null;
  }

  if (typeof value.toDate === "function") {
    return admin.firestore.Timestamp.fromDate(value.toDate());
  }

  if (value instanceof Date) {
    return admin.firestore.Timestamp.fromDate(value);
  }

  if (typeof value === "string") {
    const parsed = new Date(value);

    if (!Number.isNaN(parsed.getTime())) {
      return admin.firestore.Timestamp.fromDate(parsed);
    }
  }

  return null;
}

function normalizeTrigger(value) {
  if (typeof value !== "string") {
    return "unknown";
  }

  const normalized = value.trim().toLowerCase();
  if (!normalized) {
    return "unknown";
  }

  if (["manual"].includes(normalized)) {
    return "manual";
  }

  if (["automatic", "automatico", "auto"].includes(normalized)) {
    return "automatic";
  }

  if (["schedule", "scheduled", "agendado"].includes(normalized)) {
    return "schedule";
  }

  return "unknown";
}

async function resolveDeviceOwnerUid(deviceId) {
  const normalizedDeviceId = typeof deviceId === "string" ? deviceId.trim() : "";

  if (!normalizedDeviceId) {
    return null;
  }

  const deviceSnapshot = await db.collection("devices").doc(normalizedDeviceId).get();

  if (!deviceSnapshot.exists) {
    return null;
  }

  const ownerUid = deviceSnapshot.get("ownerUid");

  return typeof ownerUid === "string" && ownerUid.trim() ? ownerUid.trim() : null;
}

function getDeviceHistoryCollection(deviceId) {
  return db.collection("irrigationHistory").doc(deviceId).collection(HISTORY_COLLECTION_NAME);
}

async function findHistoryDocByEventId(deviceId, eventId) {
  const snapshot = await getDeviceHistoryCollection(deviceId)
    .where("eventId", "==", eventId)
    .limit(1)
    .get();

  return snapshot.empty ? null : snapshot.docs[0].ref;
}

async function writeDeviceHistoryEvent(deviceId, eventData, eventId, mergeExisting = false) {
  const deviceRootRef = db.collection("irrigationHistory").doc(deviceId);
  const deviceMetaRef = db.collection("devices").doc(deviceId);

  await deviceRootRef.set(
    {
      macAddress: deviceId,
      ownerUid: eventData.ownerUid ?? null,
      historyCollection: HISTORY_COLLECTION_NAME,
      updatedAt: admin.firestore.FieldValue.serverTimestamp(),
    },
    { merge: true }
  );

  await deviceMetaRef.set(
    {
      macAddress: deviceId,
      ownerUid: eventData.ownerUid ?? null,
      historyCollection: HISTORY_COLLECTION_NAME,
      updatedAt: admin.firestore.FieldValue.serverTimestamp(),
    },
    { merge: true }
  );

  const existingDocRef = await findHistoryDocByEventId(deviceId, eventId);
  if (existingDocRef) {
    await existingDocRef.set(
      {
        ...eventData,
        historyCollection: HISTORY_COLLECTION_NAME,
      },
      { merge: true }
    );
    return existingDocRef;
  }

  const newDocRef = getDeviceHistoryCollection(deviceId).doc(eventId);
  await newDocRef.set(
    {
      ...eventData,
      historyCollection: HISTORY_COLLECTION_NAME,
    },
    { merge: mergeExisting }
  );
  return newDocRef;
}

async function pruneDeviceHistoryToLimit(deviceId, limit = 10) {
  const normalizedDeviceId = typeof deviceId === "string" ? deviceId.trim() : "";

  if (!normalizedDeviceId || limit <= 0) {
    return;
  }

  const snapshot = await getDeviceHistoryCollection(normalizedDeviceId)
    .orderBy("startAt", "desc")
    .get();

  if (snapshot.size <= limit) {
    return;
  }

  const batch = db.batch();

  snapshot.docs.slice(limit).forEach((doc) => {
    batch.delete(doc.ref);
  });

  await batch.commit();
  console.log(`[history-prune] device=${normalizedDeviceId} removed=${snapshot.size - limit} limit=${limit}`);
}

async function fetchDeviceHistoryCollectionsByOwnerUid(ownerUid) {
  const normalizedOwnerUid = typeof ownerUid === "string" ? ownerUid.trim() : "";

  if (!normalizedOwnerUid) {
    return new Set();
  }

  const snapshot = await db.collection("devices")
    .where("ownerUid", "==", normalizedOwnerUid)
    .get();

  return new Set(snapshot.docs.map((doc) => doc.id).filter((id) => Boolean(id && id.trim())));
}

async function fetchDeviceScheduleCollectionsByOwnerUid(ownerUid) {
  return fetchDeviceHistoryCollectionsByOwnerUid(ownerUid);
}

function extractBearerToken(req) {
  const authHeader = req.headers.authorization || req.headers.Authorization;

  if (typeof authHeader !== "string") {
    return null;
  }

  const match = authHeader.match(/^Bearer\s+(.+)$/i);
  return match ? match[1] : null;
}

async function resolveAuthenticatedUid(req) {
  const idToken = extractBearerToken(req);

  if (!idToken) {
    return null;
  }

  const decoded = await admin.auth().verifyIdToken(idToken);
  return typeof decoded.uid === "string" && decoded.uid.trim() ? decoded.uid.trim() : null;
}

function belongsToUser(history, authenticatedUid) {
  if (!authenticatedUid) {
    return false;
  }

  if (typeof history.ownerUid === "string" && history.ownerUid.trim()) {
    return history.ownerUid.trim() === authenticatedUid;
  }

  return false;
}

async function belongsToAuthenticatedUserByDevice(deviceId, authenticatedUid) {
  if (!authenticatedUid || typeof deviceId !== "string" || !deviceId.trim()) {
    return false;
  }

  const ownerUid = await resolveDeviceOwnerUid(deviceId);
  return ownerUid === authenticatedUid;
}

function normalizeSchedule(data, id, deviceId = null, scheduleCollection = null) {
  return {
    id,
    deviceId: data.deviceId ?? deviceId ?? null,
    scheduleCollection: data.scheduleCollection ?? scheduleCollection ?? null,
    ativo: data.ativo ?? false,
    diasSemana: Array.isArray(data.diasSemana) ? data.diasSemana : [],
    horaAcionamento: data.horaAcionamento ?? data.hora ?? null,
    tempoAcionamento: data.tempoAcionamento ?? data.duracaoSegundos ?? null,
    createdAt: toIsoOrNull(data.createdAt),
    lastExecution: toIsoOrNull(data.lastExecution),
  };
}

function normalizeIrrigationHistory(data, id) {
  const totalVolumeLiters = data.totalVolumeLiters != null ? (Number.isFinite(Number(data.totalVolumeLiters)) ? Number(parseFloat(data.totalVolumeLiters)) : null) : null;
  const totalVolumeMl = data.totalVolumeMl != null ? (Number.isFinite(Number(data.totalVolumeMl)) ? Number(parseFloat(data.totalVolumeMl)) : null) : (totalVolumeLiters != null ? totalVolumeLiters * 1000 : null);

  return {
    id,
    deviceId: data.deviceId ?? null,
    macAddress: data.macAddress ?? data.deviceId ?? null,
    historyCollection: data.historyCollection ?? null,
    ownerUid: data.ownerUid ?? null,
    eventId: data.eventId ?? id,
    startAt: toIsoOrNull(data.startAt),
    endAt: toIsoOrNull(data.endAt),
    durationSec: data.durationSec ?? null,
    trigger: normalizeTrigger(data.trigger),
    // Motivo de encerramento: "completed" | "manual" | "no_water"
    stopReason: data.stopReason ?? "completed",
    // Derivado: false quando a irrigação foi interrompida antes do tempo (ex: reservatório vazio)
    success: data.success ?? true,
    // Flow sensor fields
    totalPulses: data.totalPulses != null ? (Number.isFinite(Number(data.totalPulses)) ? Number(data.totalPulses) : null) : null,
    totalVolumeLiters,
    totalVolumeMl,
    avgFlowRateLpm: data.avgFlowRateLpm != null ? (Number.isFinite(Number(data.avgFlowRateLpm)) ? Number(parseFloat(data.avgFlowRateLpm)) : null) : null,
    accountingVolumeMl: data.accountingVolumeMl != null ? (Number.isFinite(Number(data.accountingVolumeMl)) ? Number(parseFloat(data.accountingVolumeMl)) : null) : null,
    nominalFlowRateMlPerMin: data.nominalFlowRateMlPerMin != null ? (Number.isFinite(Number(data.nominalFlowRateMlPerMin)) ? Number(parseFloat(data.nominalFlowRateMlPerMin)) : null) : null,
    flowDetected: data.flowDetected === true || data.flowDetected === 'true' ? true : (data.flowDetected === false || data.flowDetected === 'false' ? false : null),
    flowStatus: data.flowStatus ?? null,
    createdAt: toIsoOrNull(data.createdAt),
    updatedAt: toIsoOrNull(data.updatedAt),
  };
}

function getChangedFields(before, after) {
  const fields = new Set([...Object.keys(before), ...Object.keys(after)]);
  const changed = [];

  fields.forEach((field) => {
    const beforeValue = JSON.stringify(before[field]);
    const afterValue = JSON.stringify(after[field]);

    if (beforeValue !== afterValue) {
      changed.push(field);
    }
  });

  return changed;
}

function createMqttClient() {
  const mqtt = require("mqtt");

  return mqtt.connect(MQTT_BROKER_URL, {
    username: process.env.MQTT_USERNAME,
    password: process.env.MQTT_PASSWORD,
    connectTimeout: MQTT_CONNECT_TIMEOUT_MS,
    reconnectPeriod: 0,
    clean: true,
    clientId: process.env.MQTT_CLIENT_ID || `irrigahome-fn-${Date.now()}`,
  });
}

async function publishJsonToMqtt(topic, payload) {
  if (!MQTT_BROKER_URL) {
    console.warn("MQTT_BROKER_URL nao configurado. Publicacao MQTT ignorada.");
    return;
  }

  const client = createMqttClient();

  try {
    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        reject(new Error("Timeout ao conectar no broker MQTT."));
      }, MQTT_CONNECT_TIMEOUT_MS);

      client.once("connect", () => {
        clearTimeout(timeout);
        resolve();
      });

      client.once("error", (error) => {
        clearTimeout(timeout);
        reject(error);
      });
    });

    await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        reject(new Error("Timeout ao publicar mensagem MQTT."));
      }, MQTT_PUBLISH_TIMEOUT_MS);

      client.publish(topic, JSON.stringify(payload), { qos: 1, retain: MQTT_RETAIN_MESSAGES }, (error) => {
        clearTimeout(timeout);

        if (error) {
          reject(error);
          return;
        }

        resolve();
      });
    });
  } finally {
    client.end(true);
  }
}

function scheduleCommandsTopic(deviceId) {
  const normalizedDeviceId = typeof deviceId === "string" ? deviceId.trim().toLowerCase().replace(/[:-]/g, "") : "";
  return normalizedDeviceId ? `irrigahome/${normalizedDeviceId}/commands` : MQTT_TOPIC_COMMANDS;
}

async function publishScheduleEvent(action, schedule, extra = {}) {
  const topic = scheduleCommandsTopic(schedule.deviceId);
  const payload = {
    action,
    entity: "schedule",
    timestamp: new Date().toISOString(),
    schedule,
    ...extra,
  };

  await publishJsonToMqtt(topic, payload);
  console.log(`Agendamento ${schedule.id} publicado no topico ${topic} com action ${action}.`);
}

// 🔹 Buscar agendamentos
exports.getAgendamentos = onRequest(
  { invoker: "public" }, // 👈 libera acesso (evita Forbidden)
  async (req, res) => {
    try {
      const authenticatedUid = await resolveAuthenticatedUid(req);

      if (!authenticatedUid) {
        return res.status(401).json({
          error: "Nao autenticado. Envie um Firebase ID token em Authorization: Bearer <token>",
        });
      }

      const deviceIds = await fetchDeviceScheduleCollectionsByOwnerUid(authenticatedUid);
      const docs = [];

      for (const deviceId of deviceIds) {
        for (const scheduleCollection of SCHEDULE_COLLECTIONS) {
          const snapshot = await db.collection("schedules")
            .doc(deviceId)
            .collection(scheduleCollection)
            .get();

          docs.push(...snapshot.docs);
        }
      }

      const lista = [];

      for (const doc of docs) {
        const deviceId = doc.ref.parent.parent?.id ?? null;
        const scheduleCollection = doc.ref.parent.id;
        const schedule = normalizeSchedule(doc.data(), doc.id, deviceId, scheduleCollection);
        const ownerUid = doc.get("ownerUid");

        if (typeof ownerUid === "string" && ownerUid.trim()) {
          if (ownerUid.trim() !== authenticatedUid) {
            continue;
          }
        } else {
          const matchesDeviceOwner = await belongsToAuthenticatedUserByDevice(schedule.deviceId, authenticatedUid);
          if (!matchesDeviceOwner) {
            continue;
          }
        }

        lista.push({
          id: schedule.id,
          device: schedule.deviceId,
          ativo: schedule.ativo,
          dias: schedule.diasSemana,
          hora: schedule.horaAcionamento,
          duracao: schedule.tempoAcionamento,
        });
      }

      res.status(200).json(lista);

    } catch (error) {
      console.error(error);
      res.status(500).send(error.toString());
    }
  }
);

exports.getHistoricoIrrigacao = onRequest(
  { invoker: "public" },
  async (req, res) => {
    try {
      const authenticatedUid = await resolveAuthenticatedUid(req);

      if (!authenticatedUid) {
        return res.status(401).json({
          error: "Nao autenticado. Envie um Firebase ID token em Authorization: Bearer <token>",
        });
      }

      const deviceId = typeof req.query.deviceId === "string" ? req.query.deviceId.trim() : "";
      const allowedDeviceIds = await fetchDeviceHistoryCollectionsByOwnerUid(authenticatedUid);
      const docs = [];

      if (deviceId) {
        if (!allowedDeviceIds.has(deviceId)) {
          return res.status(403).json({ error: "Dispositivo nao pertence ao usuario autenticado" });
        }

        const snapshot = await db.collection("irrigationHistory").doc(deviceId).collection(HISTORY_COLLECTION_NAME).get();
        docs.push(...snapshot.docs);
      } else {
        for (const allowedDeviceId of allowedDeviceIds) {
          const snapshot = await db.collection("irrigationHistory").doc(allowedDeviceId).collection(HISTORY_COLLECTION_NAME).get();
          docs.push(...snapshot.docs);
        }
      }

      const lista = [];

      for (const doc of docs) {
        const history = normalizeIrrigationHistory(doc.data(), doc.id);

        const canRead = belongsToUser(history, authenticatedUid)
          || await belongsToAuthenticatedUserByDevice(history.deviceId, authenticatedUid);

        if (!canRead) {
          continue;
        }

        if (deviceId && history.deviceId !== deviceId) {
          continue;
        }

        lista.push({
          id: history.id,
          device: history.deviceId,
          historyCollection: history.historyCollection,
          ownerUid: history.ownerUid,
          eventId: history.eventId,
          inicio: history.startAt,
          fim: history.endAt,
          duracao: history.durationSec,
          trigger: history.trigger,
          stopReason: history.stopReason,
          success: history.success,
          // Flow sensor summary
          totalPulses: history.totalPulses,
          totalVolumeLiters: history.totalVolumeLiters,
          totalVolumeMl: history.totalVolumeMl,
          avgFlowRateLpm: history.avgFlowRateLpm,
          accountingVolumeMl: history.accountingVolumeMl,
          nominalFlowRateMlPerMin: history.nominalFlowRateMlPerMin,
          flowDetected: history.flowDetected,
          flowStatus: history.flowStatus,
        });
      }

      lista.sort((left, right) => {
        const leftValue = left.fim ? Date.parse(left.fim) : 0;
        const rightValue = right.fim ? Date.parse(right.fim) : 0;
        return rightValue - leftValue;
      });

      res.status(200).json(lista);
    } catch (error) {
      console.error(error);
      res.status(500).send(error.toString());
    }
  }
);

exports.onScheduleCreatedPublishMqtt = onDocumentCreated(
  {
    document: "schedules/{deviceId}/{scheduleCollection}/{scheduleId}",
    retry: true,
  },
  async (event) => {
    if (!event.data) {
      console.warn("Evento de criacao sem dados do documento.");
      return;
    }

    const schedule = normalizeSchedule(
      event.data.data(),
      event.params.scheduleId,
      event.params.deviceId,
      event.params.scheduleCollection
    );

    await publishScheduleEvent("scheduleCreated", schedule, {
      eventId: event.id,
    });
  }
);

exports.onScheduleUpdatedPublishMqtt = onDocumentUpdated(
  {
    document: "schedules/{deviceId}/{scheduleCollection}/{scheduleId}",
    retry: true,
  },
  async (event) => {
    if (!event.data) {
      console.warn("Evento de atualizacao sem dados do documento.");
      return;
    }

    const before = normalizeSchedule(
      event.data.before.data(),
      event.params.scheduleId,
      event.params.deviceId,
      event.params.scheduleCollection
    );
    const after = normalizeSchedule(
      event.data.after.data(),
      event.params.scheduleId,
      event.params.deviceId,
      event.params.scheduleCollection
    );

    await publishScheduleEvent("scheduleUpdated", after, {
      eventId: event.id,
      previousSchedule: before,
      changedFields: getChangedFields(before, after),
    });
  }
);

exports.onScheduleDeletedPublishMqtt = onDocumentDeleted(
  {
    document: "schedules/{deviceId}/{scheduleCollection}/{scheduleId}",
    retry: true,
  },
  async (event) => {
    if (!event.data) {
      console.warn("Evento de exclusao sem dados do documento.");
      return;
    }

    const deletedSchedule = normalizeSchedule(
      event.data.data(),
      event.params.scheduleId,
      event.params.deviceId,
      event.params.scheduleCollection
    );

    const topic = scheduleCommandsTopic(deletedSchedule.deviceId);

    await publishJsonToMqtt(topic, {
      action: "scheduleDeleted",
      entity: "schedule",
      eventId: event.id,
      timestamp: new Date().toISOString(),
      schedule: deletedSchedule,
    });

    console.log(`Agendamento ${deletedSchedule.id} removido e publicado no topico ${topic}.`);
  }
);

// 🔹 Receber eventos de irrigação do ESP32 (via HTTP)
exports.saveIrrigationEvent = onRequest(
  { invoker: "public" },
  async (req, res) => {
    try {
      // Aceita GET e POST
      const data = req.method === "POST" ? req.body : req.query;

      // Validar campos obrigatórios
      if (!data.deviceId || !data.eventId || !data.startAt) {
        return res.status(400).json({
          error: "Campos obrigatórios faltando: deviceId, eventId, startAt",
        });
      }

      const startAt = toFirestoreTimestamp(data.startAt);
      const endAt = data.endAt ? toFirestoreTimestamp(data.endAt) : null;

      if (!startAt) {
        return res.status(400).json({
          error: "Campo startAt inválido. Use uma data ISO válida.",
        });
      }

      if (data.endAt && !endAt) {
        return res.status(400).json({
          error: "Campo endAt inválido. Use uma data ISO válida.",
        });
      }

      // Preparar documento para Firestore
      const totalVolumeLiters = data.totalVolumeLiters != null ? (Number.isFinite(Number(data.totalVolumeLiters)) ? Number(parseFloat(data.totalVolumeLiters)) : null) : null;
      const totalVolumeMl = data.totalVolumeMl != null ? (Number.isFinite(Number(data.totalVolumeMl)) ? Number(parseFloat(data.totalVolumeMl)) : null) : (totalVolumeLiters != null ? totalVolumeLiters * 1000 : null);
      const ownerUid = await resolveDeviceOwnerUid(data.deviceId);
      const emailNotificationEnabled = !(data.emailNotificationEnabled === false || data.emailNotificationEnabled === "false" || data.emailNotificationEnabled === 0 || data.emailNotificationEnabled === "0");
      const eventData = {
        deviceId: data.deviceId,
        macAddress: data.deviceId,
        eventId: data.eventId,
        startAt,
        endAt,
        durationSec: parseInt(data.durationSec) || 0,
        trigger: normalizeTrigger(data.trigger),
        // Motivo de encerramento enviado pelo firmware: "completed" | "manual" | "no_water"
        stopReason: data.stopReason || "completed",
        // Booleano explícito para facilitar queries (ex: where("success", "==", false))
        success: data.success === "true" || data.success === true,
        airHumidity: parseInt(data.airHumidity) || null,
        soilHumidity: parseInt(data.soilHumidity) || null,
        temperature: parseFloat(data.temperature) || null,
        // Flow sensor fields (if provided)
        totalPulses: data.totalPulses != null ? (Number.isFinite(Number(data.totalPulses)) ? Number(data.totalPulses) : null) : null,
        totalVolumeLiters,
        totalVolumeMl,
        avgFlowRateLpm: data.avgFlowRateLpm != null ? (Number.isFinite(Number(data.avgFlowRateLpm)) ? Number(parseFloat(data.avgFlowRateLpm)) : null) : null,
        accountingVolumeMl: data.accountingVolumeMl != null ? (Number.isFinite(Number(data.accountingVolumeMl)) ? Number(parseFloat(data.accountingVolumeMl)) : null) : null,
        nominalFlowRateMlPerMin: data.nominalFlowRateMlPerMin != null ? (Number.isFinite(Number(data.nominalFlowRateMlPerMin)) ? Number(parseFloat(data.nominalFlowRateMlPerMin)) : null) : null,
        flowDetected: data.flowDetected === true || data.flowDetected === 'true' ? true : (data.flowDetected === false || data.flowDetected === 'false' ? false : null),
        flowStatus: data.flowStatus || null,
        waterLevel: data.waterLevel || null,
        emailNotificationEnabled,
        createdAt: admin.firestore.FieldValue.serverTimestamp(),
        updatedAt: admin.firestore.FieldValue.serverTimestamp(),
        ...(ownerUid ? { ownerUid } : {}),
      };

      // Usar eventId como ID do documento
      const docRef = await writeDeviceHistoryEvent(
        data.deviceId,
        eventData,
        data.eventId,
        Boolean(data.endAt)
      );

      await pruneDeviceHistoryToLimit(data.deviceId, 10);
      
      console.log(`✅ Evento de irrigação salvo em ${data.deviceId}/${HISTORY_COLLECTION_NAME}: ${data.eventId} (${docRef.id})`);

      res.status(200).json({
        success: true,
        message: "Evento salvo com sucesso",
        eventId: data.eventId,
      });
    } catch (error) {
      console.error("Erro ao salvar evento:", error);
      res.status(500).json({
        error: error.message,
      });
    }
  }
);

// 🔹 Listener para eventos MQTT (via webhook do HiveMQ)
exports.onMqttIrrigationEvent = onRequest(
  { invoker: "public" },
  async (req, res) => {
    try {
      // HiveMQ envia payload como JSON no body
      const payload = req.body;

      console.log("📨 Evento MQTT recebido:", JSON.stringify(payload));

      // Esperar payload estruturado do ESP32
      if (!payload.eventId || !payload.startAt || !payload.deviceId) {
        return res.status(400).json({
          error: "Payload inválido. Esperado: eventId, startAt, deviceId",
          received: payload,
        });
      }

      const startAt = toFirestoreTimestamp(payload.startAt);
      const endAt = payload.endAt ? toFirestoreTimestamp(payload.endAt) : null;

      if (!startAt) {
        return res.status(400).json({
          error: "Payload inválido. startAt deve ser uma data ISO válida.",
          received: payload,
        });
      }

      if (payload.endAt && !endAt) {
        return res.status(400).json({
          error: "Payload inválido. endAt deve ser uma data ISO válida.",
          received: payload,
        });
      }

      // Salvar no Firestore
      const flowSensor = payload.flowSensor || {};
      const accounting = payload.accounting || {};
      const totalVolumeLiters = payload.totalVolumeLiters != null
        ? (Number.isFinite(Number(payload.totalVolumeLiters)) ? Number(parseFloat(payload.totalVolumeLiters)) : null)
        : (flowSensor.totalVolumeLiters != null ? (Number.isFinite(Number(flowSensor.totalVolumeLiters)) ? Number(parseFloat(flowSensor.totalVolumeLiters)) : null) : null);
      const totalVolumeMl = payload.totalVolumeMl != null
        ? (Number.isFinite(Number(payload.totalVolumeMl)) ? Number(parseFloat(payload.totalVolumeMl)) : null)
        : (flowSensor.totalVolumeMl != null
          ? (Number.isFinite(Number(flowSensor.totalVolumeMl)) ? Number(parseFloat(flowSensor.totalVolumeMl)) : null)
          : (totalVolumeLiters != null ? totalVolumeLiters * 1000 : null));
      const ownerUid = await resolveDeviceOwnerUid(payload.deviceId);
      const emailNotificationEnabled = !(payload.emailNotificationEnabled === false || payload.emailNotificationEnabled === "false" || payload.emailNotificationEnabled === 0 || payload.emailNotificationEnabled === "0");
      const eventData = {
        deviceId: payload.deviceId,
        macAddress: payload.deviceId,
        eventId: payload.eventId,
        startAt,
        endAt,
        durationSec: payload.durationSec || 0,
        trigger: normalizeTrigger(payload.trigger),
        // Motivo de encerramento vindo do payload MQTT do firmware
        stopReason: payload.stopReason || "completed",
        // No payload MQTT o campo já chega como booleano nativo
        success: payload.success === true || payload.success === "true",
        airHumidity: payload.airHumidity || null,
        soilHumidity: payload.soilHumidity || null,
        temperature: payload.temperature || null,
        // Flow sensor fields (when provided in MQTT payload)
        totalPulses: payload.totalPulses != null
          ? (Number.isFinite(Number(payload.totalPulses)) ? Number(payload.totalPulses) : null)
          : (flowSensor.totalPulses != null ? (Number.isFinite(Number(flowSensor.totalPulses)) ? Number(flowSensor.totalPulses) : null) : null),
        totalVolumeLiters,
        totalVolumeMl,
        avgFlowRateLpm: payload.avgFlowRateLpm != null
          ? (Number.isFinite(Number(payload.avgFlowRateLpm)) ? Number(parseFloat(payload.avgFlowRateLpm)) : null)
          : (flowSensor.avgFlowRateLpm != null ? (Number.isFinite(Number(flowSensor.avgFlowRateLpm)) ? Number(parseFloat(flowSensor.avgFlowRateLpm)) : null) : null),
        accountingVolumeMl: payload.accountingVolumeMl != null
          ? (Number.isFinite(Number(payload.accountingVolumeMl)) ? Number(parseFloat(payload.accountingVolumeMl)) : null)
          : (accounting.accountingVolumeMl != null ? (Number.isFinite(Number(accounting.accountingVolumeMl)) ? Number(parseFloat(accounting.accountingVolumeMl)) : null) : null),
        nominalFlowRateMlPerMin: payload.nominalFlowRateMlPerMin != null
          ? (Number.isFinite(Number(payload.nominalFlowRateMlPerMin)) ? Number(parseFloat(payload.nominalFlowRateMlPerMin)) : null)
          : (accounting.nominalFlowRateMlPerMin != null ? (Number.isFinite(Number(accounting.nominalFlowRateMlPerMin)) ? Number(parseFloat(accounting.nominalFlowRateMlPerMin)) : null) : null),
        flowDetected: payload.flowDetected === true || payload.flowDetected === 'true'
          ? true
          : (payload.flowDetected === false || payload.flowDetected === 'false'
            ? false
            : (flowSensor.flowDetected === true || flowSensor.flowDetected === 'true'
              ? true
              : (flowSensor.flowDetected === false || flowSensor.flowDetected === 'false' ? false : null))),
        flowStatus: payload.flowStatus || flowSensor.flowStatus || null,
        waterLevel: payload.waterLevel || null,
              emailNotificationEnabled,
        createdAt: admin.firestore.FieldValue.serverTimestamp(),
        updatedAt: admin.firestore.FieldValue.serverTimestamp(),
        ...(ownerUid ? { ownerUid } : {}),
      };

      await writeDeviceHistoryEvent(
        payload.deviceId,
        eventData,
        payload.eventId,
        Boolean(payload.endAt)
      );

      await pruneDeviceHistoryToLimit(payload.deviceId, 10);

      res.status(200).json({
        success: true,
        message: "Evento MQTT processado",
      });
    } catch (error) {
      console.error("Erro ao processar evento MQTT:", error);
      res.status(500).json({
        error: error.message,
      });
    }
  }
);

// 🔹 Retorna todos os agendamentos de um dispositivo (para sincronização na reconexão do ESP32).
// O ESP32 chama este endpoint logo após conectar no MQTT para reconciliar sua NVS com o Firestore.
// Autenticação: deviceId verificado contra o campo ownerUid via cabeçalho Authorization (Bearer token)
// OU via query param ?deviceId=... sem autenticação (para uso direto pelo firmware sem token).
// O firmware passa apenas deviceId; o backend retorna somente os schedules daquele device.
exports.getDeviceSchedules = onRequest(
  { invoker: "public" },
  async (req, res) => {
    try {
      const deviceId = typeof req.query.deviceId === "string" ? req.query.deviceId.trim().toLowerCase().replace(/[:-]/g, "") : "";

      if (!deviceId) {
        return res.status(400).json({ error: "Parâmetro deviceId obrigatório." });
      }

      // Coleta agendamentos em todas as sub-coleções conhecidas do device
      const docs = [];
      for (const scheduleCollection of SCHEDULE_COLLECTIONS) {
        const snapshot = await db
          .collection("schedules")
          .doc(deviceId)
          .collection(scheduleCollection)
          .get();
        docs.push(...snapshot.docs);
      }

      const lista = docs.map((doc) => {
        const d = doc.data();
        // Converte diasSemana para array de inteiros (1-7, onde 1=domingo)
        const diasSemana = Array.isArray(d.diasSemana) ? d.diasSemana.map(Number).filter((n) => !Number.isNaN(n)) : [];
        return {
          id: doc.id,
          ativo: d.ativo === true,
          diasSemana,
          horaAcionamento: d.horaAcionamento ?? d.hora ?? null,
          tempoAcionamento: d.tempoAcionamento ?? d.duracaoSegundos ?? null,
          createdAt: toIsoOrNull(d.createdAt),
        };
      }).filter((s) => s.horaAcionamento != null && s.tempoAcionamento != null);

      console.log(`[getDeviceSchedules] device=${deviceId} schedules=${lista.length}`);
      res.status(200).json({ schedules: lista });
    } catch (error) {
      console.error("[getDeviceSchedules] Erro:", error);
      res.status(500).json({ error: error.message });
    }
  }
);

// 🔹 Recuperação de senha do painel de manutenção local
// O firmware ESP32 chama este endpoint passando deviceId + email.
// A CF valida o e-mail contra o owner do dispositivo, gera uma senha
// temporária com validade de 30 min, envia por e-mail e retorna o
// hash+salt (SHA-256) para o firmware persistir na NVS.
//
// Fluxo de segurança:
//  1. Verifica se deviceId é válido e tem ownerUid
//  2. Busca o e-mail do owner em users/{ownerUid}
//  3. Compara com o e-mail informado (case-insensitive)
//  4. Gera senha temporária aleatória (8 chars hex)
//  5. Calcula hash+salt que serão persistidos no firmware
//  6. Envia e-mail via nodemailer (configurar SMTP em variáveis de ambiente)
//  7. Retorna hash+salt+expiresAt para o firmware
exports.requestMaintenancePasswordRecovery = onRequest(
  { invoker: "public" },
  async (req, res) => {
    if (req.method !== "POST") {
      return res.status(405).json({ ok: false, error: "Metodo nao permitido" });
    }

    try {
      const deviceId = typeof req.body.deviceId === "string"
        ? req.body.deviceId.trim().toLowerCase().replace(/[:-]/g, "")
        : "";
      const emailInput = typeof req.body.email === "string"
        ? req.body.email.trim().toLowerCase()
        : "";

      if (!deviceId) {
        return res.status(400).json({ ok: false, error: "deviceId obrigatorio" });
      }
      if (!emailInput || !emailInput.includes("@")) {
        return res.status(400).json({ ok: false, error: "e-mail invalido" });
      }

      // 1. Localiza ownerUid pelo deviceId
      const ownerUid = await resolveDeviceOwnerUid(deviceId);
      if (!ownerUid) {
        // Resposta genérica para não confirmar existência do dispositivo
        return res.status(200).json({ ok: false, error: "Nao foi possivel validar as informacoes. Verifique o e-mail informado." });
      }

      // 2. Busca e-mail do owner no Firestore
      const userSnap = await db.collection("users").doc(ownerUid).get();
      if (!userSnap.exists) {
        return res.status(200).json({ ok: false, error: "Nao foi possivel validar as informacoes. Verifique o e-mail informado." });
      }
      const ownerEmail = typeof userSnap.get("email") === "string"
        ? userSnap.get("email").trim().toLowerCase()
        : "";

      // 3. Compara e-mail
      if (!ownerEmail || ownerEmail !== emailInput) {
        return res.status(200).json({ ok: false, error: "Nao foi possivel validar as informacoes. Verifique o e-mail informado." });
      }

      // 4. Gera senha temporária aleatória (8 hex chars = 4 bytes)
      const crypto = require("crypto");
      const tempPassword = crypto.randomBytes(4).toString("hex"); // ex: "a3f91c7b"
      const salt         = crypto.randomBytes(16).toString("hex");

      // 5. Calcula hash SHA-256(salt + password) — mesmo algoritmo do firmware
      const hash = crypto.createHash("sha256").update(salt + tempPassword).digest("hex");

      // Expiração: 30 minutos a partir de agora
      const expiresAt = Math.floor(Date.now() / 1000) + 1800;

      // 6. Envia e-mail com a senha temporária
      // Usa nodemailer com SMTP configurado nas variáveis de ambiente:
      //   SMTP_HOST, SMTP_PORT, SMTP_USER, SMTP_PASS, SMTP_FROM
      const smtpHost = process.env.SMTP_HOST || "";
      if (smtpHost) {
        try {
          const nodemailer = require("nodemailer");
          const transporter = nodemailer.createTransport({
            host: smtpHost,
            port: parseInt(process.env.SMTP_PORT || "587", 10),
            secure: process.env.SMTP_SECURE === "true",
            auth: {
              user: process.env.SMTP_USER,
              pass: process.env.SMTP_PASS,
            },
          });
          await transporter.sendMail({
            from: process.env.SMTP_FROM || process.env.SMTP_USER,
            to: ownerEmail,
            subject: "Irriga Home — Senha temporária de manutenção",
            text: [
              "Você solicitou a recuperação da senha do painel de manutenção do seu irrigador.",
              "",
              `Senha temporária: ${tempPassword}`,
              "",
              "Validade: 30 minutos.",
              "Após o primeiro acesso, você será solicitado a definir uma nova senha.",
              "",
              "Se você não solicitou essa recuperação, ignore este e-mail.",
            ].join("\n"),
          });
          console.log(`[recovery] e-mail enviado para ${ownerEmail} device=${deviceId}`);
        } catch (mailErr) {
          console.error("[recovery] falha ao enviar e-mail:", mailErr);
          // Não aborta: retorna hash mesmo assim (o usuário pode tentar novamente)
        }
      } else {
        // SMTP não configurado: loga a senha temporária apenas no servidor (uso em desenvolvimento)
        console.warn(`[recovery] SMTP nao configurado. Senha temporaria para device=${deviceId}: ${tempPassword}`);
      }

      // 7. Retorna hash+salt+expiresAt para o firmware
      return res.status(200).json({
        ok: true,
        hash,
        salt,
        expiresAt,
      });
    } catch (error) {
      console.error("[recovery] Erro:", error);
      return res.status(500).json({ ok: false, error: error.message });
    }
  }
);

// ── Notificações por e-mail após irrigação bem-sucedida ──────────────────────
// Dispara via Firestore trigger sempre que um evento de irrigação é finalizado
// com success=true e trigger automático ou agendado.

function irrigFormatDateBR(value) {
  if (!value) return "—";
  let d;
  if (typeof value.toDate === "function") d = value.toDate();
  else if (value instanceof Date) d = value;
  else d = new Date(value);
  if (isNaN(d.getTime())) return "—";
  return d.toLocaleString("pt-BR", {
    timeZone: "America/Sao_Paulo",
    day: "2-digit", month: "2-digit", year: "numeric",
    hour: "2-digit", minute: "2-digit",
  });
}

function irrigFormatDuration(sec) {
  if (!sec || sec <= 0) return "—";
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  if (m === 0) return `${s}s`;
  if (s === 0) return `${m} min`;
  return `${m} min ${s}s`;
}

function irrigFormatVolume(ml) {
  const v = Number(ml);
  if (!ml || isNaN(v) || v <= 0) return null;
  return v >= 1000 ? `${(v / 1000).toFixed(2).replace(".", ",")} L` : `${v.toFixed(0)} mL`;
}

function buildIrrigationEmailHtml({ userName, deviceName, trigger, data }) {
  const isAuto = trigger === "automatic" || trigger === "automatico" || trigger === "auto";
  const isSchedule = trigger === "schedule";
  if (!isAuto && !isSchedule) return null;

  const startFmt = irrigFormatDateBR(data.startAt);
  const soilPct  = data.soilHumidity != null ? `${data.soilHumidity}%` : null;

  const eventIcon  = isAuto ? "\uD83C\uDF31" : "\uD83D\uDCC5";
  const eventTitle = isAuto ? "Irriga\u00E7\u00E3o autom\u00E1tica realizada" : "Irriga\u00E7\u00E3o agendada realizada";

  const reasonHtml = isAuto
    ? `<div style="background:#f0fdf4;border-left:4px solid #22c55e;border-radius:0 8px 8px 0;padding:14px 16px;margin:0 0 22px">
        <p style="margin:0;font-size:13px;line-height:1.7;color:#166534">
          \u2705 <strong>Irriga\u00E7\u00E3o conclu\u00EDda com sucesso</strong><br>
          ${soilPct ? `A umidade do solo estava em <strong>${soilPct}</strong>, valor inferior ao limite configurado.` : "A umidade do solo estava abaixo do limite configurado."}
          O sistema iniciou automaticamente a irriga\u00E7\u00E3o para manter a sa\u00FAde da vegeta\u00E7\u00E3o.
        </p>
      </div>`
    : `<div style="background:#eff6ff;border-left:4px solid #3b82f6;border-radius:0 8px 8px 0;padding:14px 16px;margin:0 0 22px">
        <p style="margin:0;font-size:13px;line-height:1.7;color:#1e40af">
          \u2705 <strong>Irriga\u00E7\u00E3o programada conclu\u00EDda com sucesso</strong><br>
          O agendamento configurado para <strong>${startFmt}</strong> foi executado normalmente.
        </p>
      </div>`;

  const tel = [];
  const addRow = (label, value) => { if (value != null && value !== "" && value !== "\u2014") tel.push([label, value]); };
  addRow("\uD83D\uDCC5 Data e hora",         startFmt);
  addRow("\u23F1\uFE0F Dura\u00E7\u00E3o",   irrigFormatDuration(data.durationSec));
  addRow("\uD83D\uDCA7 Umidade do solo",     soilPct);
  addRow("\uD83C\uDF21\uFE0F Temperatura",   data.temperature != null ? `${parseFloat(data.temperature).toFixed(1)}\u00B0C` : null);
  addRow("\uD83D\uDCA8 Umidade do ar",       data.airHumidity  != null ? `${data.airHumidity}%` : null);
  addRow("\uD83D\uDEB0 Reservat\u00F3rio",   data.waterLevel || null);
  addRow("\uD83D\uDCA6 Volume estimado",     irrigFormatVolume(data.totalVolumeMl));
  addRow("\uD83D\uDD04 Tipo de irriga\u00E7\u00E3o", isAuto ? "Autom\u00E1tica (sensor de solo)" : "Agendada");
  if (data.firmwareVersion) addRow("\u2699\uFE0F Firmware", data.firmwareVersion);

  const telRows = tel.map(([l, v]) => `
      <tr>
        <td style="padding:9px 14px;font-size:13px;color:#64748b;border-bottom:1px solid #f1f5f9;white-space:nowrap">${l}</td>
        <td style="padding:9px 14px;font-size:13px;font-weight:600;color:#1e293b;border-bottom:1px solid #f1f5f9">${v}</td>
      </tr>`).join("");

  return `<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Irriga Home \u2014 Notifica\u00E7\u00E3o de Irriga\u00E7\u00E3o</title>
</head>
<body style="margin:0;padding:0;background:#f1f5f9;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif">
<table width="100%" cellpadding="0" cellspacing="0" role="presentation" style="background:#f1f5f9;padding:32px 16px">
<tr><td align="center">
<table width="100%" style="max-width:580px;background:#fff;border-radius:16px;overflow:hidden;border:1px solid #e2e8f0" cellpadding="0" cellspacing="0">

  <tr><td style="background:#0f172a;padding:22px 28px">
    <p style="margin:0;font-size:20px;font-weight:700;color:#fff;letter-spacing:-.01em">\uD83C\uDF31 Irriga Home</p>
    <p style="margin:3px 0 0;font-size:12px;color:#94a3b8">Sistema de irriga\u00E7\u00E3o inteligente</p>
  </td></tr>

  <tr><td style="padding:28px">
    <p style="margin:0 0 6px;font-size:26px">${eventIcon}</p>
    <h1 style="margin:0 0 8px;font-size:19px;font-weight:700;color:#0f172a;line-height:1.3">${eventTitle}</h1>
    <p style="margin:0 0 22px;font-size:14px;color:#64748b;line-height:1.7">
      Ol\u00E1, <strong style="color:#1e293b">${userName || "usu\u00E1rio"}</strong>!<br>
      Seu irrigador <strong style="color:#1e293b">${deviceName}</strong> realizou uma irriga\u00E7\u00E3o com sucesso.
    </p>
    ${reasonHtml}
    <p style="margin:0 0 10px;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.07em;color:#94a3b8">Dados da opera\u00E7\u00E3o</p>
    <table width="100%" cellpadding="0" cellspacing="0" style="border:1px solid #e2e8f0;border-radius:10px;overflow:hidden">
      ${telRows}
    </table>
  </td></tr>

  <tr><td style="background:#f8fafc;padding:18px 28px;border-top:1px solid #e2e8f0">
    <p style="margin:0 0 6px;font-size:13px;color:#475569;line-height:1.7">
      Obrigado por utilizar o <strong>Irriga Home</strong>.<br>
      Estamos trabalhando para que suas plantas recebam sempre a quantidade ideal de \u00E1gua, de forma inteligente e autom\u00E1tica.
    </p>
    <p style="margin:8px 0 0;font-size:11px;color:#94a3b8">Este e-mail foi gerado automaticamente \u2014 por favor n\u00E3o responda.</p>
  </td></tr>

</table>
</td></tr>
</table>
</body>
</html>`;
}

async function sendIrrigationNotificationEmail(to, subject, html) {
  const smtpHost = process.env.SMTP_HOST || "";
  if (!smtpHost) {
    console.warn("[notify] SMTP nao configurado \u2014 notifica\u00E7\u00E3o ignorada");
    return false;
  }
  const nodemailer = require("nodemailer");
  const transporter = nodemailer.createTransport({
    host: smtpHost,
    port: parseInt(process.env.SMTP_PORT || "587", 10),
    secure: process.env.SMTP_SECURE === "true",
    auth: { user: process.env.SMTP_USER, pass: process.env.SMTP_PASS },
  });
  await transporter.sendMail({
    from: process.env.SMTP_FROM || process.env.SMTP_USER,
    to,
    subject,
    html,
  });
  return true;
}

async function resolveOwnerNameAndEmail(ownerUid) {
  if (!ownerUid) return { name: null, email: null };
  try {
    const snap = await db.collection("users").doc(ownerUid).get();
    if (!snap.exists) return { name: null, email: null };
    return { name: snap.get("name") || null, email: snap.get("email") || null };
  } catch (e) {
    console.warn("[notify] falha ao buscar dados do owner:", e.message);
    return { name: null, email: null };
  }
}

exports.onIrrigationEventNotification = onDocumentWritten(
  { document: `irrigationHistory/{deviceId}/${HISTORY_COLLECTION_NAME}/{eventId}` },
  async (event) => {
    const after  = event.data?.after?.data();
    const before = event.data?.before?.data();

    // Pré-condições: evento concluído com sucesso e endAt presente
    if (!after || after.success !== true || !after.endAt) return;
    // Deduplicação: não reenvia se o e-mail já foi enviado
    if (after.emailNotificationSentAt) return;
    const emailNotificationEnabled = !(after.emailNotificationEnabled === false || after.emailNotificationEnabled === "false" || after.emailNotificationEnabled === 0 || after.emailNotificationEnabled === "0");
    if (!emailNotificationEnabled) return;
    // Apenas irrigação automática ou agendada gera notificação
    const trigger = typeof after.trigger === "string" ? after.trigger.toLowerCase() : "";
    if (trigger !== "automatic" && trigger !== "schedule") return;
    // Dispara apenas na transição sem-endAt → com-endAt (evita reprocessar updates posteriores)
    if (before && before.endAt != null) return;

    const deviceId = event.params.deviceId;
    const ownerUid = after.ownerUid || await resolveDeviceOwnerUid(deviceId);
    if (!ownerUid) {
      console.warn(`[notify] ownerUid nao resolvido device=${deviceId}`);
      return;
    }

    const { name, email } = await resolveOwnerNameAndEmail(ownerUid);
    if (!email) {
      console.warn(`[notify] e-mail do owner nao encontrado uid=${ownerUid}`);
      return;
    }

    // Resolve nome amigável do dispositivo
    let deviceName = deviceId;
    try {
      const devSnap = await db.collection("users").doc(ownerUid).collection("devices").doc(deviceId).get();
      const stored  = devSnap.get("deviceName") || devSnap.get("name");
      if (stored) deviceName = stored;
    } catch { /* usa deviceId como fallback */ }

    const html = buildIrrigationEmailHtml({ userName: name, deviceName, trigger, data: after });
    if (!html) return;

    const trigLabel = trigger === "automatic" ? "Autom\u00E1tica" : "Agendada";
    const subject   = `\uD83C\uDF31 Irriga Home \u2014 Irriga\u00E7\u00E3o ${trigLabel} realizada com sucesso`;

    try {
      const sent = await sendIrrigationNotificationEmail(email, subject, html);
      if (sent) {
        console.log(`[notify] e-mail enviado para ${email} device=${deviceId} trigger=${trigger}`);
        // Persiste flag para evitar reenvio em caso de retrigger do Firestore
        await event.data.after.ref.update({
          emailNotificationSentAt: admin.firestore.FieldValue.serverTimestamp(),
        });
      }
    } catch (err) {
      console.error(`[notify] erro ao enviar e-mail device=${deviceId}:`, err.message);
    }
  }
);