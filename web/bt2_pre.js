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
//  - Changes are stored right after a file is written (not only every few seconds), a failed
//    write is reported to the page (which warns and tries again), and a lost connection to the
//    browser's storage is opened again.

// the page reads the mixed sound through this (always the current memory view)
Module['bt2HeapF32'] = function () { return HEAPF32; };
Module['bt2HeapU8'] = function () { return HEAPU8; }; // (files and the Modland list to the program)
Module['bt2StorageOK'] = false;

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
	if (typeof document === 'undefined')
		return; // audio worklet / worker scopes: nothing to do

	Module['bt2FS'] = FS;

	// ---- remember what the user deletes/renames in this session (only those may be removed from storage)
	var deleted = [];
	var USER = '/home/web_user';
	var inUser = function (p) { return p === USER || p.indexOf(USER + '/') === 0; };
	// something in the user's files changed: store it soon
	var changed = function (p) {
		if (!inUser(p)) return;
		Module['bt2Dirty'] = true;
		if (globalThis.BT2 && globalThis.BT2.syncSoon) globalThis.BT2.syncSoon();
	};
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
			changed(p);
			return r;
		};
	};
	wrap('unlink', function (a) { return a[0]; });
	wrap('rmdir', function (a) { return a[0]; });
	wrap('rename', function (a) { return a[0]; }); // the old name disappears
	Module['bt2Deleted'] = deleted;

	// files written (or renamed to) in this session: the user's latest work, stored even if the stored copy
	// looks newer by the clock (the device's clock may have been corrected since)
	var writtenHere = {};

	// a renamed file/folder gets "now" as its time, so it is always stored under the new name
	// (an older stored file with that name, deleted earlier in this session, must not win)
	var origRename = FS.rename;
	FS.rename = function (oldPath, newPath) {
		var r = origRename.apply(this, arguments);
		try {
			var now = Date.now();
			var touch = function (p) {
				FS.utime(p, now, now);
				writtenHere[p] = true;
				if (FS.isDir(FS.stat(p).mode))
					FS.readdir(p).forEach(function (n) { if (n !== '.' && n !== '..') touch(p + '/' + n); });
			};
			touch(absPath(newPath));
		} catch (e) {}
		changed(absPath(newPath));
		return r;
	};

	// other changes: written files (closed after writing), new folders, changed times
	var origClose = FS.close;
	FS.close = function (stream) {
		var writing = stream && stream.path && (stream.flags & 3) !== 0; // opened for writing (O_WRONLY/O_RDWR)
		var r = origClose.apply(this, arguments);
		if (writing) { writtenHere[absPath(stream.path)] = true; changed(absPath(stream.path)); }
		return r;
	};
	['mkdir', 'truncate', 'utime', 'chmod', 'symlink'].forEach(function (name) {
		var orig = FS[name];
		if (!orig) return;
		FS[name] = function (p) {
			var r = orig.apply(this, arguments);
			if (typeof p === 'string') changed(absPath(name === 'symlink' ? arguments[1] : p));
			return r;
		};
	});

	// (the Modland library, /Modland, is filled in folder by folder by the program: bt2_modland.c)

	// ---- the connection to IndexedDB: when the browser closes it (Safari: "Connection to Indexed
	// Database server lost", e.g. after a while in the background), forget it so the next save opens a new one
	var origGetDB = IDBFS.getDB;
	IDBFS.getDB = function (name, callback) {
		origGetDB(name, function (err, db) {
			if (!err && db && !db.bt2Watched) {
				db.bt2Watched = true;
				var forget = function () { if (IDBFS.dbs[name] === db) delete IDBFS.dbs[name]; };
				db.onclose = forget;
				db.onversionchange = function () { try { db.close(); } catch (e) {} forget(); };
			}
			callback(err, db);
		});
	};
	Module['bt2ForgetDB'] = function () {
		Object.keys(IDBFS.dbs).forEach(function (k) { try { IDBFS.dbs[k].close(); } catch (e) {} delete IDBFS.dbs[k]; });
	};

	// ---- the transfer between memory and IndexedDB. Like Emscripten's own, but it always reports
	// back exactly once: when the transaction is done, fails, or is aborted (e.g. the storage is full).
	// Emscripten's version never reports an abort, and then nothing would be stored for the rest of
	// the session.
	// When writing to IndexedDB, files are stored first, and deletions (also the old name of a renamed
	// file) happen in a second step, only if every file was stored. A failed write (full storage) can
	// then never leave a file in neither place: the worst case is an extra copy under the old name.
	var transfer = function (src, dst, callback, stillCurrent) {
		var create = [], remove = [];
		Object.keys(src.entries).forEach(function (key) {
			var e = src.entries[key], e2 = dst.entries[key];
			if (!e2 || e.timestamp.getTime() != e2.timestamp.getTime()) create.push(key);
		});
		Object.keys(dst.entries).forEach(function (key) { if (!src.entries[key]) remove.push(key); });
		if (!create.length && !remove.length) return callback(null);

		var db = src.type === 'remote' ? src.db : dst.db;
		var run = function (work, done) { // one transaction, reports exactly once
			var finished = false, firstError = null;
			var finish = function (err) { if (!finished) { finished = true; done(err || null); } };
			var note = function (err) { if (err && !firstError) firstError = err; };
			var transaction;
			try {
				transaction = db.transaction([IDBFS.DB_STORE_NAME], 'readwrite');
			} catch (e) {
				return finish(e); // e.g. the connection was closed
			}
			var store = transaction.objectStore(IDBFS.DB_STORE_NAME);
			transaction.oncomplete = function () { finish(firstError); };
			transaction.onabort = function () { finish(transaction.error || firstError || new Error('IndexedDB transaction aborted')); };
			transaction.onerror = function (e) { note((e && e.target && e.target.error) || transaction.error); if (e) e.preventDefault(); };
			try {
				work(store, note);
			} catch (e) {
				note(e);
				try { transaction.abort(); } catch (e2) { finish(e); }
			}
		};
		var creates = function (store, note) {
			// folders before the files in them
			create.sort().forEach(function (path) {
				if (dst.type === 'local') {
					IDBFS.loadRemoteEntry(store, path, function (err, entry) {
						if (err) return note(err);
						IDBFS.storeLocalEntry(path, entry, note);
					});
				} else {
					IDBFS.loadLocalEntry(path, function (err, entry) {
						if (err) return note(err);
						// a file's data is often a view on a bigger buffer (room to grow): store only the file's bytes
						var c = entry.contents;
						if (c && c.buffer && c.byteLength !== c.buffer.byteLength) entry.contents = c.slice();
						IDBFS.storeRemoteEntry(store, path, entry, note);
					});
				}
			});
		};
		var removes = function (store, note) {
			// files before their folders
			remove.sort().reverse().forEach(function (path) {
				if (dst.type === 'local') IDBFS.removeLocalEntry(path, note);
				else IDBFS.removeRemoteEntry(store, path, note);
			});
		};

		if (dst.type === 'local') // reading the stored files (startup): one go
			return run(function (store, note) { creates(store, note); removes(store, note); }, callback);

		// (a save that took so long that a newer one was started meanwhile deletes nothing: its view is old)
		var removeIfCurrent = function () {
			if (stillCurrent && !stillCurrent()) return callback(new Error('replaced by a newer save'));
			run(removes, callback);
		};
		if (!create.length) return removeIfCurrent();
		run(creates, function (err) {
			if (err || !remove.length) return callback(err);
			removeIfCurrent();
		});
	};

	// ---- safer write-back to IndexedDB
	IDBFS.reconcile = function (src, dst, callback) {
		if (dst.type !== 'remote')
			return transfer(src, dst, callback); // reading from storage: everything as stored

		// files open for writing right now (a WAV being rendered, or a save the program stopped in the
		// middle of) are not stored until they are closed: a half-written file must not replace a good copy
		var openForWriting = {};
		(FS.streams || []).forEach(function (st) {
			if (st && st.path && (st.flags & 3) !== 0) openForWriting[absPath(st.path)] = true;
		});

		var entries = {};
		Object.keys(src.entries).forEach(function (k) {
			if (openForWriting[k]) {
				if (dst.entries[k]) entries[k] = dst.entries[k]; // keep what is stored
				return;
			}
			entries[k] = src.entries[k];
		});
		Object.keys(dst.entries).forEach(function (k) {
			var local = src.entries[k], stored = dst.entries[k];
			if (!local) {
				if (!wasDeleted(k))
					entries[k] = stored; // not here, but not deleted by the user either: keep it
			} else if (!openForWriting[k] && !writtenHere[k] && local.timestamp && stored.timestamp &&
				local.timestamp.getTime() < stored.timestamp.getTime()) {
				entries[k] = stored; // stored copy is newer (e.g. saved in another tab): keep it
			}
		});

		var gen = Module['bt2SyncGen'];
		return transfer({ type: src.type, entries: entries }, dst, callback, function () { return Module['bt2SyncGen'] === gen; });
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
