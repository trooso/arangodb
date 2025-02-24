/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual, assertFalse, assertTrue, assertNotNull */

// //////////////////////////////////////////////////////////////////////////////
// / @brief tests for revision trees
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Jan Steemann
// / @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

'use strict';
const fs = require('fs');
const db = require('@arangodb').db;
const _ = require('lodash');
const internal = require('internal');
const time = internal.time;
const jsunity = require('jsunity');

const colName = 'UnitTestsRecovery';
// number of collections
let numberOfCollections = 1;
// max number of restarts/iterations
let maxIterations = 5;
// maximum runtime for each iteration
let maxRunTime = 20;
  
const stateFile = require("internal").env['state-file'];
let iteration = 1;
try {
  iteration = parseInt(String(fs.readFileSync(stateFile)));
} catch (err) {
  // it does not matter if the file does not exist
}

function runSetup (config) {
  // create missing collections
  for (let i = 0; i < numberOfCollections; ++i) {
    if (db._collection(colName + i) === null) {
      db._create(colName + i);
    }
  }
  if (db._collection("control") === null) {
    db._create("control");
  }
  
  // test runtime is also dynamic
  let runTime = 5 + Math.random() * (maxRunTime - 5);

  require("console").warn("summoning chaos for " + runTime.toFixed(2) + " seconds...");

  let collectionCounts = {};
  for (let i = 0; i < numberOfCollections; ++i) {
    let c = db._collection(colName + i);
    if (c.count() !== c.toArray().length) {
      throw "invalid collection counts for collection " + c.name();
    }
    collectionCounts[c.name()] = c.count();
  }
  
  db.control.insert({ _key: "working", collectionCounts }, { overwriteMode: "replace" });

  require("console").warn("Collection counts from control:", JSON.stringify(collectionCounts));

  const failurePoints = [
    "rebuildRevisionTree::sleep",
    "TransactionChaos::randomSleep",
    "applyUpdates::forceHibernation1",
    "applyUpdates::forceHibernation2",
    "RevisionTree::applyInserts",
    "RevisionTree::applyRemoves",
    "serializeMeta::delayCallToLastSerializedRevisionTree",
    "needToPersistRevisionTree::checkBuffers",
    "RocksDBMetaCollection::forceSerialization",
    "RocksDBMetaCollection::serializeRevisionTree",
    "TransactionChaos::blockerOnSync",
  ];

  try {
    // number of failure points currently set
    let fpSet = 0;
    const end = time() + runTime;
    do {
      if (config.withFailurePoints) {
      let fp = Math.random();
      if (fpSet > 0 && fp >= 0.95) {
        // remove all failure points
        console.warn("clearing all failure points");
        internal.debugClearFailAt();
        fpSet = 0;
      } else if (fp >= 0.85 || fpSet === 0) {
        // set failure points
        _.shuffle(failurePoints);
        console.warn("clearing all failure points");
        internal.debugClearFailAt();
        let fp = Math.floor(Math.random() * failurePoints.length);
        for (let i = 0; i < fp; ++i) {
          internal.debugSetFailAt(failurePoints[i]);
        }
        fpSet = fp;
      }
    }

      // pick a random collection
      const cn = Math.floor(Math.random() * numberOfCollections);
      const c = db._collection(colName + cn);

      const opType = Math.random();
      let numOps = Math.floor(1000 * Math.random());
      if (opType >= 0.75) {
        // multi-document insert
        let docs = [];
        for (let i = 0; i < numOps; ++i) {
          docs.push({ value: i, valueString: String(i), type: "multi" });
        }
        c.insert(docs);
        collectionCounts[c.name()] += numOps;
      } else if (opType >= 0.5) {
        // single doc inserts
        for (let i = 0; i < numOps; ++i) {
          c.insert({ value: i, valueString: String(i), type: "individual" });
        }
        collectionCounts[c.name()] += numOps;
      } else if (opType >= 0.3) {
        // single operation insert
        c.insert({ value: 666, valueString: "666", type: "single" });
        collectionCounts[c.name()] += 1;
      } else if (opType >= 0.05) {
        // remove
        numOps /= 2;
        let keys = db._query("FOR doc IN " + c.name() + " LIMIT @numOps RETURN doc._key", { numOps }).toArray();
        if (opType >= 0.15) {
          // multi-document remove
          c.remove(keys);
        } else {
          keys.forEach((key) => {
            c.remove(key);
          });
        }
        collectionCounts[c.name()] -= keys.length;
      } else {
        // truncate
        c.truncate();
        collectionCounts[c.name()] = 0;
      }
    } while (time() < end);
   
    console.warn("storing collection counts at exit:", collectionCounts);
    db.control.insert({ _key: "counts", collectionCounts }, { overwriteMode: "replace" });
    db.control.remove("working", { waitForSync: true });
  } catch (err) {
    console.warn(String(err), String(err.stack));
    console.warn(collectionCounts);
    throw err;
  }

  require("console").warn("Collection counts written to control:", JSON.stringify(collectionCounts));

  internal.debugTerminate('crashing server');
}

function recoverySuite () {
  jsunity.jsUnity.attachAssertions();
  
  if (iteration < maxIterations) {
    ++iteration;
    // update iteration counter
    fs.writeFileSync(stateFile, String(iteration));
  } else {
    // 10 iterations done, now remove state file
    fs.remove(stateFile);
  }

  return {
    setUp: function () {
      internal.waitForEstimatorSync(); // make sure estimates are consistent
    },

    testConsistency: function() {
      const counts = db.control.document("counts");
      assertNotNull(counts);

      let toArray = (c) => {
        return db._query("FOR doc IN @@cn RETURN 1", { "@cn": c.name() }).toArray().length;
      };

      for (let i = 0; i < numberOfCollections; ++i) {
        let c = db._collection(colName + i);
        let c1 = c.count();
        let c2 = toArray(c);
        let stored = counts.collectionCounts[c.name()];
        let rev = c._revisionTreeSummary().count;
        let ver = c._revisionTreeVerification();
        ver.computed.nodes = "<reduced>";
        ver.stored.nodes = "<reduced>";
        console.warn("collection " + c.name() + ": count: " + c1 + ", toArray: " + c2 + ", stored: " + stored + ", rev count: " + rev);
        if (!ver.equal) {
          console.warn("collection " + c.name() + JSON.stringify(ver));
        }
        assertTrue(counts.collectionCounts.hasOwnProperty(c.name()), c.name());
        assertEqual(c1, c2, c.name());
        assertEqual(c1, stored, c.name());
        assertEqual(rev, c1, c.name());
        assertTrue(ver.equal, c.name());
      }

      ["_queues", "_statistics", "_statistics15", "_statisticsRaw"].forEach((colName) => {
        let c = db._collection(colName);
        assertEqual(c.count(), toArray(c));
        assertEqual(c._revisionTreeSummary().count, c.count());
        assertTrue(c._revisionTreeVerification().equal);
      });
    },

  };
}

function main (argv, config) {
  numberOfCollections = config.numberOfCollections || numberOfCollections;
  maxIterations = config.maxIterations || maxIterations;
  maxRunTime = config.maxRunTime || maxRunTime;

  if (argv[1] === 'setup') {
    runSetup(config);
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}

module.exports.main = main;
