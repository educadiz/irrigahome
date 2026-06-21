const { onRequest } = require("firebase-functions/v2/https");
const { onDocumentCreated, onDocumentUpdated, onDocumentDeleted } = require("firebase-functions/v2/firestore");
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
    trigger: data.trigger ?? null,
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
      const eventData = {
        deviceId: data.deviceId,
        macAddress: data.deviceId,
        eventId: data.eventId,
        startAt,
        endAt,
        durationSec: parseInt(data.durationSec) || 0,
        trigger: data.trigger || "unknown",
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
      const eventData = {
        deviceId: payload.deviceId,
        macAddress: payload.deviceId,
        eventId: payload.eventId,
        startAt,
        endAt,
        durationSec: payload.durationSec || 0,
        trigger: payload.trigger || "unknown",
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