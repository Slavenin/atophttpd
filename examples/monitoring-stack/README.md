# atop process traffic in monitoring-stack

Эти файлы рассчитаны на `/home/max/programms/monitoring-stack`. Существующие Prometheus, Grafana, process-exporter и node-exporter остаются в `compose.yml`; добавляются `atop-collector`, `atophttpd` и job `atophttpd`. Дашборд Grafana здесь не меняется.

`atop-collector` собирает трафик процессов через локальный `netatop-bpf` и записывает rawlog atop 2.13.0 каждые 10 секунд. `atophttpd` читает этот том и отдаёт `/metrics` Prometheus. Сборщик запускает `netatop` перед `atop -K`, иначе заголовок нового rawlog не получит признак доступной сетевой статистики. Супервизор перезапускается Docker, если дочерний процесс завершается или rawlog перестаёт расти. Он создаёт новый файл при смене даты UTC, удаляет только свои старые файлы через 7 дней и завершает только собственные дочерние процессы.

Используется уже существующий локальный `netatop-bpf` (форк с поддержкой ядра этой машины). Dockerfile патчит его копию **только внутри сборки**: демон остаётся в foreground, ошибка подключения BPF завершает процесс, а две копии старого `libbpf` получают совместимую с glibc 2.43 константность указателя. Исходники форка на хосте не меняются. Образы строятся из локального `/home/max/projects/atophttpd` и `./netatop-bpf`; клонировать другие репозитории для этого варианта не нужно. При другом расположении исходников скопируйте `.env.example` в `.env` и задайте `LOCAL_ATOPHTTPD_SOURCE` и `LOCAL_NETATOP_SOURCE`.

## Запуск и проверка

Выполнить из `/home/max/programms/monitoring-stack` после копирования файлов примера в одноимённые пути:

```sh
docker compose config --quiet
docker compose build atop-collector atophttpd
docker compose up -d atop-collector atophttpd
docker inspect --format '{{.State.Health.Status}}' atop-collector atophttpd
curl -fsS http://127.0.0.1:9080/metrics | grep '^atophttpd_\(sample_fresh\|process_network_available\)'
docker run --rm -v "$PWD/prometheus/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
  --entrypoint promtool prom/prometheus:latest \
  check config /etc/prometheus/prometheus.yml
docker kill --signal=HUP prometheus
```

После первой полной записи ожидаются `atophttpd_sample_fresh 1`, `atophttpd_process_network_available 1`, а у target `atophttpd` в Prometheus `up=1`. Healthcheck экспортёра обращается к короткому `/health`: HTTP 200 означает свежий сэмпл с поддержкой сетевой статистики, HTTP 503 означает отсутствие, устаревание или ошибку чтения. Он не рендерит весь `/metrics`. Флаг доступности берётся из заголовка rawlog и сам по себе не подтверждает, что BPF-сборщик ещё работает. За этим отдельно следит healthcheck контейнера `atop-collector`. Порт `9080` опубликован только на `127.0.0.1` хоста; Prometheus обращается к `atophttpd:2867` внутри `monitoring-network`.

В существующую панель Grafana добавьте два PromQL-запроса:

```promql
sum by (instance, name) (atophttpd_process_network_receive_bytes_per_second)
sum by (instance, name) (atophttpd_process_network_transmit_bytes_per_second)
```

Это уже значения в байтах/с, `rate()` к ним не применяют. Для отдельных PID сгруппируйте по `(instance, pid, name, start_time)`; счётчики процессов могут включать завершившиеся за интервал процессы. Проверять лучше процессом, который передаёт данные дольше 10 секунд. Короткий запрос может целиком попасть между двумя сэмплами. Для проверки перезапуска: `docker compose restart atop-collector`; дождитесь healthy, затем проверьте новый rawlog и метрики. Смена файла при перезапуске ожидаема.

Если сборщик не стартует, смотрите `docker compose logs atop-collector`. Контейнер использует host PID и UTS namespaces для системных PID и имени хоста, но отдельную сеть и IPC namespace. Ему доступны только `BPF`, `PERFMON`, `SYS_RESOURCE`; загрузка BPF зависит от ядра и этого форка. В контейнер не ставятся host-пакеты и не запускается `install_net.sh`. Общие сетевые метрики хоста остаются у node-exporter.

## Откат

Удалите job `atophttpd` из `prometheus/prometheus.yml`, проверьте конфиг через `promtool` и отправьте Prometheus `SIGHUP`. Затем остановите только новые сервисы:

```sh
docker compose stop atophttpd atop-collector
docker compose rm -f atophttpd atop-collector
```

Именованный том `atop-logs` с rawlog остаётся. Остальные контейнеры мониторинга останавливать не требуется.

## Файлы и тесты

`atop-collector/supervisor.py` управляет двумя дочерними процессами, ротацией и сроком хранения; `health.py` сверяет PID/starttime, сокет и возраст активного rawlog по атомарному `/run/collector-state.json`. `atop-collector/patch_netatop.py` проверяет точные места патча исходника во время Docker build. `atophttpd/Dockerfile` собирает экспортёр без TLS для внутренней сети Compose. Конфигурация: `ATOP_INTERVAL_SECONDS`, `ATOP_RETENTION_DAYS`, `LOCAL_ATOPHTTPD_SOURCE`, `LOCAL_NETATOP_SOURCE`. Локальный тест супервизора без BPF:

```sh
python3 -m unittest discover -s atop-collector -p 'test_*.py' -v
```
