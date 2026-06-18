import argparse
import json
import random
import time

import paho.mqtt.client as mqtt


def build_payload(
    cycle_id: int,
    gateway_id: int,
    relay_id: int,
    runner_id: int,
    latitude: float,
    longitude: float,
    seq: int,
) -> str:
    pace = round(random.uniform(4.8, 6.4), 2)
    battery = max(1, 95 - seq)
    rssi = random.randint(-98, -72)
    snr = round(random.uniform(5.0, 10.0), 2)
    return json.dumps(
        {
            "cycle_id": cycle_id,
            "gateway_id": gateway_id,
            "relay_id": relay_id,
            "runner_id": runner_id,
            "lat": round(latitude, 6),
            "lng": round(longitude, 6),
            "pace": pace,
            "battery": battery,
            "seq": seq,
            "runner_rssi": rssi,
            "runner_snr": snr,
            "gw_rssi": random.randint(-95, -70),
            "gw_snr": round(random.uniform(5.0, 10.0), 2),
        },
        separators=(",", ":"),
    )


def build_emergency_payload(
    emergency_id: int,
    gateway_id: int,
    relay_id: int,
    runner_id: int,
    latitude: float,
    longitude: float,
    seq: int,
) -> str:
    return json.dumps(
        {
            "emergency_id": emergency_id,
            "gateway_id": gateway_id,
            "relay_id": relay_id,
            "runner_id": runner_id,
            "lat": round(latitude, 6),
            "lng": round(longitude, 6),
            "battery": max(1, 95 - seq),
            "gps_valid": 1,
            "rssi": random.randint(-98, -72),
            "snr": round(random.uniform(5.0, 10.0), 2),
            "seq": seq,
        },
        separators=(",", ":"),
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Publish fake Gateway JSON runner data/emergency messages to MQTT."
    )
    parser.add_argument("--host", default="localhost")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument(
        "--mode",
        choices=["data", "emergency", "both"],
        default="data",
        help="Message type to publish.",
    )
    parser.add_argument(
        "--topic",
        default=None,
        help="Data topic override. Defaults to marathon/gateways/{gateway_id}/data.",
    )
    parser.add_argument(
        "--emergency-topic",
        default=None,
        help="Emergency topic override. Defaults to marathon/gateways/{gateway_id}/emergency.",
    )
    parser.add_argument(
        "--runner-ids",
        default="7,8,9",
        help="Comma-separated runner ids, for example: 7,8,9",
    )
    parser.add_argument("--gateway-id", type=int, default=1)
    parser.add_argument("--relay-id", type=int, default=1)
    parser.add_argument("--cycle-id", type=int, default=1)
    parser.add_argument("--emergency-id", type=int, default=1)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--lat", type=float, default=36.10321)
    parser.add_argument("--lng", type=float, default=129.38712)
    parser.add_argument(
        "--step",
        type=float,
        default=0.00005,
        help="Coordinate delta per tick. Increase this to make the path longer.",
    )
    parser.add_argument(
        "--connect-delay",
        type=float,
        default=0.5,
        help="Seconds to wait after MQTT connect before first publish.",
    )
    args = parser.parse_args()

    runner_ids = parse_runner_ids(args.runner_ids)
    data_topic = args.topic or f"marathon/gateways/{args.gateway_id}/data"
    emergency_topic = (
        args.emergency_topic
        or f"marathon/gateways/{args.gateway_id}/emergency"
    )

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.connect(args.host, args.port, 60)
    client.loop_start()
    time.sleep(args.connect_delay)

    print(f"mqtt broker: {args.host}:{args.port}")
    print(f"mode: {args.mode}")
    if args.mode in {"data", "both"}:
        print(f"data topic: {data_topic}")
    if args.mode in {"emergency", "both"}:
        print(f"emergency topic: {emergency_topic}")
    print(f"runner ids: {runner_ids}")
    print(f"messages: {args.count} ticks x {len(runner_ids)} runners")

    try:
        for seq in range(1, args.count + 1):
            for index, runner_id in enumerate(runner_ids):
                latitude = args.lat + (index * 0.00035) + (seq * args.step)
                longitude = args.lng + (index * 0.00030) + (seq * args.step)
                if args.mode in {"data", "both"}:
                    payload = build_payload(
                        args.cycle_id,
                        args.gateway_id,
                        args.relay_id,
                        runner_id,
                        latitude,
                        longitude,
                        seq,
                    )
                    result = client.publish(data_topic, payload)
                    result.wait_for_publish()
                    print(f"published -> {data_topic}: {payload}")

                if args.mode in {"emergency", "both"}:
                    emergency_payload = build_emergency_payload(
                        args.emergency_id,
                        args.gateway_id,
                        args.relay_id,
                        runner_id,
                        latitude,
                        longitude,
                        seq,
                    )
                    result = client.publish(emergency_topic, emergency_payload)
                    result.wait_for_publish()
                    print(f"published -> {emergency_topic}: {emergency_payload}")
            time.sleep(args.interval)
    finally:
        client.loop_stop()
        client.disconnect()


def parse_runner_ids(value: str) -> list[int]:
    runner_ids: list[int] = []
    for raw_runner_id in value.split(","):
        raw_runner_id = raw_runner_id.strip()
        if not raw_runner_id:
            continue
        runner_ids.append(int(raw_runner_id))

    if not runner_ids:
        raise ValueError("--runner-ids must contain at least one runner id")

    return runner_ids


if __name__ == "__main__":
    main()
