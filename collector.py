import requests
import json
import os
import typing
import time
import sys
from datetime import datetime
from concurrent.futures import ThreadPoolExecutor, as_completed

class Camera:
    def __init__(self, name, base_dir):
        self.url = f"http://{name}.local"
        self.name = name
        self.base_dir = base_dir
        self.image_dir = os.path.join(base_dir, "image")
        self.data_dir = os.path.join(base_dir, "data")

        for d in [self.base_dir, self.image_dir, self.data_dir]:
            if not os.path.exists(d):
                os.makedirs(d)

    def get_image(self):
        try:
            response = requests.get(f"{self.url}/image", timeout=2.5)
            response.raise_for_status()

            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"{self.name}_{timestamp}.jpg"
            filepath = os.path.join(self.image_dir, filename)

            with open(filepath, 'wb') as f:
                f.write(response.content)

            return filepath
        except Exception as e:
            print(f"[{self.name}] Error fetching image: {e}", file=sys.stderr)
            return None

    def get_bulk_data(self):
        try:
            response = requests.get(f"{self.url}/sensor/bulk", timeout=2.5)
            response.raise_for_status()
            data = response.json()

            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"{self.name}_{timestamp}_bulk.json"
            filepath = os.path.join(self.data_dir, filename)

            with open(filepath, 'w') as f:
                json.dump(data, f, indent=4)
            
            return data
        except Exception as e:
            print(f"[{self.name}] Error fetching bulk data: {e}", file=sys.stderr)
            return None

    def get_latest_data(self):
        try:
            response = requests.get(f"{self.url}/sensor/latest", timeout=2.5)
            response.raise_for_status()
            return response.json()
        except Exception as e:
            print(f"[{self.name}] Error fetching latest data: {e}", file=sys.stderr)
            return None


def print_bar():
    print("-----------------------------------------------------------------")


def collect_all(camera: Camera):
    res = {
        "camera": camera.name,
        "img": "",
        "bulk": [],
        "latest": [],
    }

    with ThreadPoolExecutor(max_workers=3) as executor:
        r1 = executor.submit(camera.get_image)
        r2 = executor.submit(camera.get_bulk_data)
        r3 = executor.submit(camera.get_latest_data)

        to_res = {
            r1: "img",
            r2: "bulk",
            r3: "latest",
        }

        for r in as_completed([r1, r2, r3]):
            if r.result() != None:
                res[to_res[r]] = r.result()
            elif to_res[r] == "img":
                # Losing sensor data might be OK.. confirm this later
                return {}

    return res


if __name__ == "__main__":
    while True:
        base_dir = "/home/team2/test_data"
        names = ["cam1", "cam2", "cam3", "cam4"] 
        cameras = [Camera(name, base_dir) for name in names]

        delay_next = False
        with ThreadPoolExecutor(max_workers=12) as main_executor:
            futures = {main_executor.submit(collect_all, c) for c in cameras}

            for r in as_completed(futures):
                if (r.result() == {}): continue
                print_bar()
                for k,v in r.result().items():
                    print(f"{k}: {v}")
                print("")
                delay_next = True

        if delay_next:
            delay_next = False
            # Prevent next pull for 30s
            time.sleep(30)
        else:
            time.sleep(3.5)
