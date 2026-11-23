import argparse
import csv
import datetime
import getpass
import json
import sys

FIELDS = ("uptime_ms", "actual", "smooth", "trend", "target", "running", "heater", "state", "reason", "pulse_ms")
HEADER = ["time", *FIELDS]


def main():
    parser = argparse.ArgumentParser(description="Monitor ReflowPlate MQTT status messages.")
    parser.add_argument("host", help="MQTT broker hostname or IP address")
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--topic", default="reflowplate/status")
    parser.add_argument("--user", help="MQTT username; prompts for the password")
    parser.add_argument("--csv", help="Append readings to this CSV file")
    args = parser.parse_args()

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        parser.exit(1, 'Install paho-mqtt first: python -m pip install "paho-mqtt>=2,<3"\n')

    output = None
    writer = None
    if args.csv:
        output = open(args.csv, "a+", newline="", encoding="utf-8")
        output.seek(0)
        existing = next(csv.reader(output), None)
        if existing is not None and existing != HEADER:
            output.close()
            parser.exit(1, f"{args.csv} uses a different column layout; choose a new CSV file.\n")
        writer = csv.writer(output)
        if existing is None:
            writer.writerow(HEADER)
            output.flush()

    def on_connect(client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            print(f"MQTT connection failed: {reason_code}", file=sys.stderr, flush=True)
            return
        client.subscribe(args.topic)
        print(f"Subscribed: {args.topic}", flush=True)

    def on_message(client, userdata, message):
        try:
            data = json.loads(message.payload.decode("utf-8"))
            if not isinstance(data, dict):
                raise ValueError("expected a JSON object")
            timestamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
            values = [data.get(key) for key in FIELDS]
            print(f"{timestamp} {json.dumps(data, ensure_ascii=False)}", flush=True)
            if writer:
                writer.writerow([timestamp, *values])
                output.flush()
        except (UnicodeDecodeError, ValueError, TypeError) as error:
            print(f"Invalid MQTT message: {error}", file=sys.stderr, flush=True)

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    if args.user:
        client.username_pw_set(args.user, getpass.getpass("MQTT password: "))

    try:
        client.connect(args.host, args.port, keepalive=60)
        client.loop_forever(retry_first_connection=True)
    except KeyboardInterrupt:
        print("\nStopped.")
    except OSError as error:
        print(f"MQTT connection error: {error}", file=sys.stderr)
        return 1
    finally:
        client.disconnect()
        if output:
            output.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())