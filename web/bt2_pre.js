// Bernttracker II (web): runs inside the program's JavaScript before main().
// Mounts the user's own storage (IndexedDB) as /home/web_user so songs, samples,
// folders and settings are kept between sessions.
//
// SAFETY RULES for the user's files (they may contain many hours of work):
//  - Nothing is ever written back to storage unless the stored files were read in completely
//    at startup (Module.bt2StorageOK). Otherwise an empty/partial view could replace them.
//  - Writing back never deletes a stored file or folder that was not deleted by the user in
//    this session (Disk Op "Delete"/"Rename"), and never replaces a stored file with an older
//    copy (e.g. the app open in two tabs at the same time).

// the page reads the mixed sound through this (always the current memory view)
Module['bt2HeapF32'] = function () { return HEAPF32; };
Module['bt2StorageOK'] = false;

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
	if (typeof document === 'undefined')
		return; // audio worklet / worker scopes: nothing to do

	Module['bt2FS'] = FS;

	// ---- remember what the user deletes/renames in this session (only those may be removed from storage)
	var deleted = [];
	var absPath = function (p) {
		p = String(p);
		if (p.charAt(0) !== '/')
			p = FS.cwd() + '/' + p;

		var out = [];
		p.split('/').forEach(function (part) {
			if (part === '' || part === '.') return;
			if (part === '..') out.pop(); else out.push(part);
		});
		return '/' + out.join('/');
	};
	var wasDeleted = function (key) {
		for (var i = 0; i < deleted.length; i++) {
			var d = deleted[i];
			if (key === d || key.indexOf(d + '/') === 0)
				return true;
		}
		return false;
	};
	var wrap = function (name, getPath) {
		var orig = FS[name];
		FS[name] = function () {
			var p = absPath(getPath(arguments));
			var r = orig.apply(this, arguments);
			deleted.push(p);
			return r;
		};
	};
	wrap('unlink', function (a) { return a[0]; });
	wrap('rmdir', function (a) { return a[0]; });
	wrap('rename', function (a) { return a[0]; }); // the old name disappears
	Module['bt2Deleted'] = deleted;

	// ---- the Modland library (/Modland) is filled in folder by folder, when a folder is first listed
	var origReaddir = FS.readdir;
	FS.readdir = function (path) {
		try {
			if (globalThis.BT2 && globalThis.BT2.fsReaddirHook)
				globalThis.BT2.fsReaddirHook(absPath(path));
		} catch (e) {}
		return origReaddir.apply(this, arguments);
	};

	// ---- safer write-back to IndexedDB
	var origReconcile = IDBFS.reconcile;
	IDBFS.reconcile = function (src, dst, callback) {
		if (dst.type !== 'remote')
			return origReconcile.apply(this, arguments); // reading from storage: unchanged

		var entries = {};
		Object.keys(src.entries).forEach(function (k) { entries[k] = src.entries[k]; });
		Object.keys(dst.entries).forEach(function (k) {
			var local = src.entries[k], stored = dst.entries[k];
			if (!local) {
				if (!wasDeleted(k))
					entries[k] = stored; // not here, but not deleted by the user either: keep it
			} else if (local.timestamp && stored.timestamp && local.timestamp.getTime() < stored.timestamp.getTime()) {
				entries[k] = stored; // stored copy is newer: keep it
			}
		});

		return origReconcile.call(this, { type: src.type, entries: entries }, dst, callback);
	};

	FS.mount(IDBFS, {}, '/home/web_user');

	var stage = function (t) { if (globalThis.BT2 && globalThis.BT2.loadStage) globalThis.BT2.loadStage(t); };
	stage('Henter lagrede filer…');
	addRunDependency('bt2-storage');

	// Safari sometimes doesn't answer the first IndexedDB request after a page load until it
	// is "poked" again. Poke it until it answers, then load the stored files.
	var idbReady = function (done) {
		if (typeof indexedDB === 'undefined' || !indexedDB.databases) { done(); return; }
		var finished = false;
		var finish = function () { if (!finished) { finished = true; clearInterval(timer); done(); } };
		var poke = function () { try { indexedDB.databases().then(finish, finish); } catch (e) { finish(); } };
		var timer = setInterval(poke, 100);
		poke();
		setTimeout(finish, 3000);
	};

	var tries = 0;
	var load = function () {
		FS.syncfs(true, function (err) {
			if (err && ++tries < 3) {
				console.warn('Could not load stored files, trying again:', err);
				setTimeout(load, 500);
				return;
			}

			if (err) {
				// Keep going, but never write back (that could replace the stored files with nothing)
				console.error('Could not load stored files:', err);
				if (globalThis.BT2 && globalThis.BT2.storageFailed) globalThis.BT2.storageFailed(String(err));
			} else {
				Module['bt2StorageOK'] = true;
			}

			try { FS.chdir('/home/web_user'); } catch (e) {}
			stage('Starter…');
			removeRunDependency('bt2-storage');
		});
	};

	idbReady(load);
});
