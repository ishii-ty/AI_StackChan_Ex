let currentDir = "/";

const breadcrumbEl = document.getElementById("breadcrumb");
const upButton = document.getElementById("upButton");
const fileListEl = document.getElementById("fileList");
const uploadForm = document.getElementById("uploadForm");
const uploadInput = document.getElementById("uploadInput");
const statusEl = document.getElementById("status");
const errorEl = document.getElementById("error");

function showStatus(msg) {
  statusEl.textContent = msg;
  errorEl.textContent = "";
}

function showError(msg) {
  errorEl.textContent = msg;
  statusEl.textContent = "";
}

function joinPath(dir, name) {
  if (dir === "/") return "/" + name;
  return dir + "/" + name;
}

function parentPath(dir) {
  if (dir === "/" || dir === "") return "/";
  const idx = dir.lastIndexOf("/");
  if (idx <= 0) return "/";
  return dir.substring(0, idx);
}

function formatSize(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  return (bytes / (1024 * 1024)).toFixed(1) + " MB";
}

async function loadList(dir) {
  currentDir = dir;
  breadcrumbEl.textContent = dir;
  fileListEl.innerHTML = "";
  try {
    const res = await fetch("/sd/list?dir=" + encodeURIComponent(dir));
    if (!res.ok) {
      showError("一覧取得に失敗しました: " + res.status);
      return;
    }
    const entries = await res.json();
    entries.sort((a, b) => {
      if (a.isDir !== b.isDir) return a.isDir ? -1 : 1;
      return a.name.localeCompare(b.name);
    });
    for (const entry of entries) {
      const tr = document.createElement("tr");

      const nameTd = document.createElement("td");
      nameTd.className = "name";
      nameTd.textContent = (entry.isDir ? "[DIR] " : "") + entry.name;
      const path = joinPath(dir, entry.name);
      if (entry.isDir) {
        nameTd.addEventListener("click", () => loadList(path));
      }
      tr.appendChild(nameTd);

      const sizeTd = document.createElement("td");
      sizeTd.className = "size";
      sizeTd.textContent = entry.isDir ? "" : formatSize(entry.size);
      tr.appendChild(sizeTd);

      const actionTd = document.createElement("td");
      if (!entry.isDir) {
        const a = document.createElement("a");
        a.className = "download";
        a.href = "/sd/download?path=" + encodeURIComponent(path);
        a.textContent = "DL";
        actionTd.appendChild(a);
      }
      const delButton = document.createElement("button");
      delButton.className = "action";
      delButton.textContent = "削除";
      delButton.addEventListener("click", () => deleteEntry(path));
      actionTd.appendChild(delButton);
      tr.appendChild(actionTd);

      fileListEl.appendChild(tr);
    }
    showStatus("");
  } catch (e) {
    showError("一覧取得に失敗しました: " + e);
  }
}

async function deleteEntry(path) {
  if (!confirm(path + " を削除しますか？")) return;
  try {
    const body = new URLSearchParams();
    body.append("path", path);
    const res = await fetch("/sd/delete", { method: "POST", body });
    if (res.ok) {
      showStatus("削除しました: " + path);
      loadList(currentDir);
    } else {
      showError("削除に失敗しました: " + res.status);
    }
  } catch (e) {
    showError("削除に失敗しました: " + e);
  }
}

upButton.addEventListener("click", () => {
  loadList(parentPath(currentDir));
});

uploadForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const file = uploadInput.files[0];
  if (!file) {
    showError("ファイルを選択してください");
    return;
  }
  const formData = new FormData();
  formData.append("file", file);
  try {
    const res = await fetch("/sd/upload?dir=" + encodeURIComponent(currentDir), {
      method: "POST",
      body: formData,
    });
    if (res.ok) {
      showStatus("アップロードしました: " + file.name);
      uploadInput.value = "";
      loadList(currentDir);
    } else {
      showError("アップロードに失敗しました: " + res.status);
    }
  } catch (e) {
    showError("アップロードに失敗しました: " + e);
  }
});

loadList(currentDir);
