import os

import deploy

start = os.environ["NB_LOG_START"]
end = os.environ["NB_LOG_END"]
keywords = tuple(value.lower() for value in os.environ.get("NB_LOG_KEYWORDS", "").split(",") if value)

for role in ("entry", "middle", "exit"):
    connection = deploy.connect(role)
    try:
        content = deploy.run(connection, f"tail -30000 /etc/NB/logs/nb-{role}.log", tmo=60)
    finally:
        connection.close()
    print(f"=== {role} ===")
    for line in content.splitlines():
        timestamp = line[1:25] if line.startswith("[") else ""
        if start <= timestamp <= end and (not keywords or any(key in line.lower() for key in keywords)):
            print(line)
