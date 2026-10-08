# Подпись релизов через SignPath Foundation

Сборка уже готова к подписи: на тег `v*` workflow [build.yml](../.github/workflows/build.yml)
отправляет Windows-пакет в SignPath и выкладывает в релиз подписанный архив. Пока SignPath не
настроен, релиз выходит без подписи.

## Что сделать владельцу репозитория (один раз)

1. Включить двухфакторную аутентификацию на GitHub (условие SignPath Foundation для всей команды).
2. Подать заявку на бесплатную подпись: <https://signpath.org/apply> (репозиторий, лицензия GPL-3.0,
   уже вышедший релиз, этот раздел и «Code signing policy» в README).
3. После одобрения в SignPath.io:
   - проект со slug `PS5CameraDriver`;
   - trusted build system **GitHub.com**, связанный с проектом; установить
     [SignPath GitHub App](https://github.com/apps/signpath) для этого репозитория;
   - artifact configuration из [artifact-configuration.xml](artifact-configuration.xml);
   - signing policy `release-signing` с ручным одобрением;
   - API-токен пользователя CI с правом отправлять запросы на подпись.
4. В настройках репозитория GitHub (Settings → Secrets and variables → Actions):
   - секрет `SIGNPATH_API_TOKEN` — токен из шага 3;
   - переменная `SIGNPATH_ORGANIZATION_ID` — ID организации в SignPath;
   - при других slug — переменные `SIGNPATH_PROJECT_SLUG` и `SIGNPATH_POLICY_SLUG`.

После этого каждый тег `v*` создаёт запрос на подпись; его нужно одобрить в SignPath, и сборка
продолжится сама (ожидание до суток).
