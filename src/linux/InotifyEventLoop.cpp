#include "../../includes/linux/InotifyEventLoop.h"
#include <iostream>

InotifyEventLoop::InotifyEventLoop(
  int inotifyInstance,
  InotifyService *inotifyService
) :
  mInotifyInstance(inotifyInstance),
  mInotifyService(inotifyService)
{
  if (pthread_mutex_init(&mMutex, NULL) != 0) {
    mStarted = false;
    return;
  }

  mStarted = !pthread_create(
    &mEventLoop,
    NULL,
    [](void *eventLoop)->void * {
      ((InotifyEventLoop *)eventLoop)->work();
    },
    (void *)this
  );

}

void InotifyEventLoop::work() {
  char buffer[8192];
  inotify_event *event = NULL;
  unsigned int bytesRead, position = 0, i;
  bool isDirectoryEvent = false, isDirectoryRemoval = false;
  InotifyService *inotifyService = mInotifyService;
  InotifyRenameEvent *renameEvent = NULL;

  auto create = [&event, &isDirectoryEvent, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    if (isDirectoryEvent) {
      inotifyService->createDirectory(event->wd, strdup(event->name));
    } else {
      inotifyService->create(event->wd, strdup(event->name));
    }
  };

  auto modify = [&event, &isDirectoryEvent, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    inotifyService->modify(event->wd, strdup(event->name));
  };

  auto remove = [&event, &isDirectoryRemoval, &inotifyService]() {
    if (event == NULL) {
      return;
    }

    if (isDirectoryRemoval) {
      inotifyService->removeDirectory(event->wd);
    } else {
      inotifyService->remove(event->wd, strdup(event->name));
    }
  };

  auto renameStart = [&event, &isDirectoryEvent, &renameEvent]() {
    if (renameEvent == NULL) {
      renameEvent = new InotifyRenameEvent;
    }

    renameEvent->cookie = event->cookie;
    renameEvent->isDirectory = isDirectoryEvent;
    renameEvent->name = event->name;
    renameEvent->wd = event->wd;
  };

  auto renameEnd = [&create, &event, &inotifyService, &isDirectoryEvent, &renameEvent]() {
    if (renameEvent == NULL) {
      create();
      return;
    }

    if (renameEvent->cookie != event->cookie) {
      if (renameEvent->isDirectory) {
        inotifyService->removeDirectory(renameEvent->wd);
      } else {
        inotifyService->remove(renameEvent->wd, renameEvent->name);
      }
      create();
    } else {
      if (renameEvent->isDirectory) {
        inotifyService->renameDirectory(renameEvent->wd, renameEvent->name, event->name);
      } else {
        inotifyService->rename(renameEvent->wd, renameEvent->name, event->name);
      }
    }
    delete renameEvent;
    renameEvent = NULL;
  };

  while((bytesRead = read(mInotifyInstance, &buffer, 8192)) > 0) {
    Lock syncWithDestructor(this->mMutex);
    do {
      event = (struct inotify_event *)(buffer + position);
      isDirectoryRemoval = event->mask & (uint32_t)(IN_IGNORED | IN_DELETE_SELF);
      isDirectoryEvent = event->mask & (uint32_t)(IN_ISDIR);

      if (!isDirectoryRemoval && *event->name <= 31) {
        continue;
      }

      if (event->mask & (uint32_t)(IN_ATTRIB | IN_MODIFY)) {
        modify();
      } else if (event->mask & (uint32_t)IN_CREATE) {
        create();
      } else if (event->mask & (uint32_t)(IN_DELETE | IN_DELETE_SELF)) {
        remove();
      } else if (event->mask & (uint32_t)IN_MOVED_TO) {
        if (event->cookie == 0) {
          create();
          continue;
        }

        renameEnd();
      } else if (event->mask & (uint32_t)IN_MOVED_FROM) {
        if (event->cookie == 0) {
          remove();
          continue;
        }

        renameStart();
      }
    } while((position += sizeof(struct inotify_event) + event->len) < bytesRead);
    position = 0;
  }
}

InotifyEventLoop::~InotifyEventLoop() {
  if (!mStarted) {
    return;
  }

  {
    Lock syncWithWork(this->mMutex);
    pthread_cancel(mEventLoop);
  }

  pthread_mutex_destroy(&mMutex);
}
