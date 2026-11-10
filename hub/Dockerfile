# One image for both uses:
#  - the Home Assistant add-on (the Supervisor passes BUILD_FROM, an HA base
#    image, from build.yaml), and
#  - plain Docker anywhere else (docker build hub/ uses python:3.13-alpine).
ARG BUILD_FROM=python:3.13-alpine
FROM ${BUILD_FROM}

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY had_hub ./had_hub

# S6_KEEP_ENV: under the HA base images' s6 /init, CMD otherwise starts with
# an empty environment - no SUPERVISOR_TOKEN, no HA_URL.
ENV PYTHONUNBUFFERED=1 S6_KEEP_ENV=1
EXPOSE 8390
# The HA base images start through s6's /init, which then runs this.
CMD ["python3", "-m", "had_hub"]

LABEL io.hass.type="addon" \
      org.opencontainers.image.title="Display Hub" \
      org.opencontainers.image.source="https://github.com/geekychris/HomeAutomationDisplay"
