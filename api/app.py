"""ECSE3038 Test 2: Fan registration, humidity readings and MQTT control."""
import json
import os
import re
import secrets
from datetime import datetime, timezone
from pathlib import Path
from typing import Literal

import paho.mqtt.client as mqtt
from dotenv import load_dotenv
from fastapi import FastAPI, Header, HTTPException, Query, Response
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field
from pymongo import MongoClient, DESCENDING
from pymongo.errors import DuplicateKeyError

load_dotenv(Path(__file__).parent / ".env")
MONGODB_URI = os.getenv("MONGODB_URI")
MQTT_NAME = os.getenv("MQTT_NAME", "")
MQTT_HOST = "135.232.203.144"
MQTT_PORT = 1883

if not MONGODB_URI:
    raise RuntimeError("MONGODB_URI is missing from api/.env")
if not re.fullmatch(r"[a-z0-9]+", MQTT_NAME):
    raise RuntimeError("MQTT_NAME must contain lowercase letters and digits only")

mongo = MongoClient(MONGODB_URI, tz_aware=True)
database = mongo["ecse3038_test2"]
fans = database["fans"]
readings = database["readings"]
fans.create_index("mac", unique=True)
fans.create_index("api_key", unique=True)

app = FastAPI(title="ECSE3038 Test 2 - Bathroom Fan API")


class FanInput(BaseModel):
    mac: str = Field(min_length=1)
    room: str = Field(min_length=1)


class ReadingInput(BaseModel):
    mac: str = Field(min_length=1)
    humidity: float = Field(ge=0, le=100)


class SettingsInput(BaseModel):
    mode: Literal["auto", "on", "off"]
    max_humidity: float = Field(ge=0, le=100)


def public_fan(record):
    return {k: v for k, v in record.items() if k not in ("_id", "api_key")}


def require_key_for_mac(mac: str, provided_key: str | None):
    if not provided_key:
        raise HTTPException(status_code=401, detail="Missing API key")
    owner = fans.find_one({"api_key": provided_key})
    if owner is None:
        raise HTTPException(status_code=401, detail="Unknown API key")
    if owner["mac"] != mac:
        raise HTTPException(status_code=403, detail="API key belongs to another fan")
    return owner


def publish_settings(payload: dict):
    topic = f"ecse3038/{MQTT_NAME}/fan/settings"
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        client.loop_start()
        info = client.publish(topic, json.dumps(payload), qos=1, retain=True)
        info.wait_for_publish(timeout=5)
        if not info.is_published():
            raise RuntimeError("MQTT publish was not confirmed")
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"MQTT publish failed: {exc}") from exc
    finally:
        try:
            client.disconnect()
            client.loop_stop()
        except Exception:
            pass


@app.post("/fans", status_code=201)
def register_fan(body: FanInput):
    api_key = secrets.token_urlsafe(16)
    record = {"mac": body.mac, "room": body.room, "api_key": api_key}
    try:
        fans.insert_one(record)
    except DuplicateKeyError:
        raise HTTPException(status_code=409, detail="MAC already registered")
    return {"mac": body.mac, "room": body.room, "api_key": api_key}


@app.get("/fans")
def list_fans():
    return [public_fan(fan) for fan in fans.find()]


@app.get("/fans/{mac}")
def get_fan(mac: str):
    record = fans.find_one({"mac": mac})
    if record is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    return public_fan(record)


@app.post("/readings", status_code=201)
def add_reading(body: ReadingInput, x_api_key: str | None = Header(default=None, alias="X-API-Key")):
    require_key_for_mac(body.mac, x_api_key)
    record = {
        "mac": body.mac,
        "humidity": body.humidity,
        "timestamp": datetime.now(timezone.utc),
    }
    readings.insert_one(record)
    return {
        "mac": record["mac"],
        "humidity": record["humidity"],
        "timestamp": record["timestamp"].isoformat(),
    }


@app.get("/readings")
def list_readings(mac: str | None = None, limit: int = Query(default=10, ge=1, le=100)):
    selector = {"mac": mac} if mac is not None else {}
    records = readings.find(selector, {"_id": 0}).sort("timestamp", DESCENDING).limit(limit)
    return [
        {**record, "timestamp": record["timestamp"].isoformat()}
        for record in records
    ]


@app.put("/fans/{mac}/settings")
def put_settings(mac: str, body: SettingsInput):
    existing = fans.find_one({"mac": mac})
    if existing is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    payload = {"mode": body.mode, "max_humidity": body.max_humidity}
    fans.update_one({"mac": mac}, {"$set": {"settings": payload}})
    publish_settings(payload)
    status_code = 200 if "settings" in existing else 201
    return JSONResponse(status_code=status_code, content=payload)


@app.delete("/fans/{mac}", status_code=204)
def delete_fan(mac: str, x_api_key: str | None = Header(default=None, alias="X-API-Key")):
    record = fans.find_one({"mac": mac})
    if record is None:
        raise HTTPException(status_code=404, detail="Fan not found")
    require_key_for_mac(mac, x_api_key)
    fans.delete_one({"mac": mac})
    return Response(status_code=204)
