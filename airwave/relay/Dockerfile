# Airwave relay in a container: ffmpeg + numpy (video dithering) + yt-dlp with node (YouTube).
#   docker build -t airwave-relay relay/ && docker run -p 8787:8787 -e AIRWAVE_TOKEN=change-me airwave-relay
# The same image is what the Cloudflare Containers kit in ../cloudflare deploys.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg nodejs ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir numpy yt-dlp
WORKDIR /app
COPY relay.py /app/relay.py
ENV PORT=8787
EXPOSE 8787
CMD ["python3", "relay.py", "--host", "0.0.0.0"]
